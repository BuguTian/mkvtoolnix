/*
   mkvmerge -- utility for splicing together matroska files
   from component media subtypes

   Distributed under the GPL
   see the file COPYING for details
   or visit http://www.gnu.org/copyleft/gpl.html

   $Id$

   The cluster helper groups frames into blocks groups and those
   into clusters, sets the durations, renders the clusters etc.

   Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#include "os.h"

#include <cassert>
#include <limits.h>

#include <vector>

#include <matroska/KaxBlock.h>
#include <matroska/KaxBlockData.h>
#include <matroska/KaxSeekHead.h>

#include "cluster_helper.h"
#include "common.h"
#include "commonebml.h"
#include "hacks.h"
#include "libmatroska_extensions.h"
#include "output_control.h"
#include "p_video.h"
#include "p_vorbis.h"

// #define walk_clusters() check_clusters(__LINE__)
#define walk_clusters()

cluster_helper_c::cluster_helper_c():
  min_timecode_in_cluster(-1), max_timecode_in_cluster(-1),
  current_split_point(split_points.begin()) {

  cluster_content_size = 0;
  max_timecode_and_duration = 0;
  last_cluster_tc = 0;
  num_cue_elements = 0;
  header_overhead = -1;
  packet_num = 0;
  out = NULL;
  timecode_offset = 0;
  last_packets = NULL;
  first_timecode_in_file = -1;
  bytes_in_file = 0;
}

cluster_helper_c::~cluster_helper_c() {
  int i;

  for (i = 0; i < clusters.size(); i++)
    delete clusters[i];
}

KaxCluster *
cluster_helper_c::get_cluster() {
  if (clusters.size() > 0)
    return clusters.back()->cluster;
  return NULL;
}

void
cluster_helper_c::add_packet(packet_cptr packet) {
  ch_contents_t *c;
  int64_t timecode, timecode_delay, additional_size;
  int i;
  bool split;

  // Normalize the timecodes according to the timecode scale.
  packet->unmodified_assigned_timecode = packet->assigned_timecode;
  packet->unmodified_duration = packet->duration;
  packet->timecode = RND_TIMECODE_SCALE(packet->timecode);
  if (packet->duration > 0)
    packet->duration = RND_TIMECODE_SCALE(packet->duration);
  packet->assigned_timecode = RND_TIMECODE_SCALE(packet->assigned_timecode);
  if (packet->bref > 0)
    packet->bref = RND_TIMECODE_SCALE(packet->bref);
  if (packet->fref > 0)
    packet->fref = RND_TIMECODE_SCALE(packet->fref);

  timecode = get_timecode();

  timecode_delay =
    (packet->assigned_timecode > max_timecode_in_cluster) ||
    (-1 == max_timecode_in_cluster) ?
    packet->assigned_timecode : max_timecode_in_cluster;
  timecode_delay -=
    (-1 == min_timecode_in_cluster) ||
    (packet->assigned_timecode < min_timecode_in_cluster) ?
    packet->assigned_timecode : min_timecode_in_cluster;
  timecode_delay = (int64_t)(timecode_delay / timecode_scale);

  mxverb(4, "cluster_helper_c::add_packet(): new packet { source " LLD "/%s "
         "timecode: " LLD " duration: " LLD " bref: " LLD " fref: " LLD " "
         "assigned_timecode: " LLD " timecode_delay: " FMT_TIMECODEN " }\n",
         packet->source->ti.id, packet->source->ti.fname.c_str(),
         packet->timecode, packet->duration, packet->bref, packet->fref,
         packet->assigned_timecode, ARG_TIMECODEN(timecode_delay));

  if (clusters.size() == 0)
    add_cluster(new kax_cluster_c());
  else if ((SHRT_MAX < timecode_delay) || (SHRT_MIN > timecode_delay) ||
           (packet->gap_following && (clusters.back()->packets.size() != 0)) ||
           (((packet->assigned_timecode - timecode) > max_ns_per_cluster) &&
            all_references_resolved(clusters.back()))) {
    render();
    add_cluster(new kax_cluster_c());
  }

  if (splitting() &&
      (split_points.end() != current_split_point) &&
      (file_num <= split_max_num_files) &&
      (packet->bref == -1) &&
      ((packet->source->get_track_type() == track_video) ||
       (video_packetizer == NULL))) {
    split = false;
    c = clusters[clusters.size() - 1];

    // Maybe we want to start a new file now.
    if (split_point_t::SPT_SIZE == current_split_point->m_type) {

      if (c->packets.size() > 0) {
        // Cluster + Cluster timecode (roughly)
        additional_size = 21;
        // Add sizes for all frames.
        for (i = 0; i < c->packets.size(); i++) {
          packet_cptr &p = c->packets[i];
          additional_size += p->data->get_size();
          if (p->bref == -1)
            additional_size += 10;
          else if (p->fref == -1)
            additional_size += 13;
          else
            additional_size += 16;
        }
      } else
        additional_size = 0;
      if (num_cue_elements > 0) {
        kax_cues->UpdateSize();
        additional_size += kax_cues->ElementSize();
      }
      mxverb(3, "cluster_helper split decision: header_overhead: " LLD ", "
             "additional_size: " LLD ", bytes_in_file: " LLD ", sum: " LLD
             "\n", header_overhead, additional_size, bytes_in_file,
             header_overhead + additional_size + bytes_in_file);
      if ((header_overhead + additional_size + bytes_in_file) >=
          current_split_point->m_point)
        split = true;

    } else if ((split_point_t::SPT_DURATION == current_split_point->m_type) &&
               (0 <= first_timecode_in_file) &&
               (packet->assigned_timecode - first_timecode_in_file) >=
               current_split_point->m_point)
      split = true;

    else if ((split_point_t::SPT_TIMECODE == current_split_point->m_type) &&
             (packet->assigned_timecode >= current_split_point->m_point))
      split = true;

    if (split) {
      render();

      num_cue_elements = 0;

      mxinfo("\n");
      finish_file();
      create_next_output_file();
      if (no_linking)
        last_cluster_tc = 0;
      add_cluster(new kax_cluster_c());

      bytes_in_file = 0;
      first_timecode_in_file = -1;

      if (no_linking)
        timecode_offset = packet->assigned_timecode;

      if (current_split_point->m_use_once)
        ++current_split_point;
    }
  }

  packet->packet_num = packet_num;
  packet_num++;

  c = clusters.back();
  c->packets.push_back(packet);
  cluster_content_size += packet->data->get_size();

  walk_clusters();

  if (packet->assigned_timecode > max_timecode_in_cluster)
    max_timecode_in_cluster = packet->assigned_timecode;
  if ((-1 == min_timecode_in_cluster) ||
      (packet->assigned_timecode < min_timecode_in_cluster))
    min_timecode_in_cluster = packet->assigned_timecode;

  // Render the cluster if it is full (according to my many criteria).
  timecode = get_timecode();
  if ((((packet->assigned_timecode - timecode) > max_ns_per_cluster) ||
       (get_packet_count() > max_blocks_per_cluster) ||
       (get_cluster_content_size() > 1500000)) &&
      all_references_resolved(c)) {
    render();
    add_cluster(new kax_cluster_c());
  }
}

bool
cluster_helper_c::all_references_resolved(ch_contents_t *cluster) {
  int i;

  for (i = 0; i < cluster->packets.size(); i++) {
    packet_cptr &pack = cluster->packets[i];
    if ((pack->bref != -1) &&
        (find_packet(pack->bref, pack->source).get() == NULL))
      return false;
    if ((pack->fref != -1) &&
        (find_packet(pack->fref, pack->source).get() == NULL))
      return false;
  }

  return true;
}

int64_t
cluster_helper_c::get_timecode() {
  if (clusters.size() == 0)
    return 0;
  if (clusters.back()->packets.size() == 0)
    return 0;
  return clusters.back()->packets[0]->assigned_timecode;
}

packet_cptr
cluster_helper_c::get_packet(int num) {
  ch_contents_t *c;

  if (clusters.size() == 0)
    return packet_cptr(NULL);
  c = clusters.back();
  if (c->packets.size())
    return packet_cptr(NULL);
  if ((num < 0) || (num > c->packets.size()))
    return packet_cptr(NULL);
  return c->packets[num];
}

int
cluster_helper_c::get_packet_count() {
  if (clusters.size() == 0)
    return -1;
  return clusters.back()->packets.size();
}

int
cluster_helper_c::find_cluster(KaxCluster *cluster) {
  int i;

  if (clusters.size() == 0)
    return -1;
  for (i = 0; i < clusters.size(); i++)
    if (clusters[i]->cluster == cluster)
      return i;
  return -1;
}

void
cluster_helper_c::add_cluster(KaxCluster *cluster) {
  ch_contents_t *c;

  if (find_cluster(cluster) != -1)
    return;
  c = new ch_contents_t;
  clusters.push_back(c);
  c->cluster = cluster;
  cluster_content_size = 0;
  cluster->SetParent(*kax_segment);
  cluster->SetPreviousTimecode(last_cluster_tc, (int64_t)timecode_scale);
}

int
cluster_helper_c::get_cluster_content_size() {
  return cluster_content_size;
}

void
cluster_helper_c::set_output(mm_io_c *nout) {
  out = nout;
}

void
cluster_helper_c::set_duration(render_groups_t *rg) {
  uint32_t i;
  int64_t block_duration, def_duration;
  kax_block_blob_c *group;

  if (rg->durations.size() == 0)
    return;

  group = rg->groups.back();
  block_duration = 0;
  for (i = 0; i < rg->durations.size(); i++)
    block_duration += rg->durations[i];
  def_duration = rg->source->get_track_default_duration();
  mxverb(3, "cluster_helper::set_duration: block_duration " LLD " "
         "rounded duration " LLD " def_duration "
         "" LLD " use_durations %d rg->duration_mandatory %d\n",
         block_duration, RND_TIMECODE_SCALE(block_duration), def_duration,
         use_durations ? 1 : 0, rg->duration_mandatory ? 1 : 0);

  if (rg->duration_mandatory) {
    if ((block_duration == 0) ||
        ((block_duration > 0) &&
         (block_duration != (rg->durations.size() * def_duration))))
      group->set_block_duration(RND_TIMECODE_SCALE(block_duration));
  } else if ((use_durations || (def_duration > 0)) &&
             (block_duration > 0) &&
             (RND_TIMECODE_SCALE(block_duration) !=
              RND_TIMECODE_SCALE(rg->durations.size() * def_duration)))
    group->set_block_duration(RND_TIMECODE_SCALE(block_duration));
}

bool
cluster_helper_c::must_duration_be_set(render_groups_t *rg,
                                       packet_cptr &new_packet) {
  uint32_t i;
  int64_t block_duration, def_duration;

  block_duration = 0;
  for (i = 0; i < rg->durations.size(); i++)
    block_duration += rg->durations[i];
  block_duration += new_packet->duration;
  def_duration = rg->source->get_track_default_duration();

  if (rg->duration_mandatory || new_packet->duration_mandatory) {
    if ((block_duration == 0) ||
        ((block_duration > 0) &&
         (block_duration != ((rg->durations.size() + 1) * def_duration))))
      return true;
  } else if ((use_durations || (def_duration > 0)) &&
             (block_duration > 0) &&
             (RND_TIMECODE_SCALE(block_duration) !=
              RND_TIMECODE_SCALE((rg->durations.size() + 1) * def_duration)))
    return true;

  return false;
}

/*
  <+Asylum> The chicken and the egg are lying in bed next to each
            other after a good hard shag, the chicken is smoking a
            cigarette, the egg says "Well that answers that bloody
            question doesn't it"
*/

int
cluster_helper_c::render(bool flush) {
  if (clusters.size() == 0)
    return 0;

  walk_clusters();
  return render_cluster(clusters.back());
}

int
cluster_helper_c::render_cluster(ch_contents_t *clstr) {
  KaxCluster *cluster;
  kax_block_blob_c *new_block_group, *last_block_group;
  DataBuffer *data_buffer;
  int i, k, elements_in_cluster;
  packet_cptr pack;
  int64_t max_cl_timecode;
  generic_packetizer_c *source;
  vector<render_groups_t *> render_groups;
  render_groups_t *render_group;
  bool added_to_cues, has_codec_state;
  LacingType lacing_type;
  bool use_simpleblock = hack_engaged(ENGAGE_USE_SIMPLE_BLOCK);
  BlockBlobType this_block_blob_type, std_block_blob_type = use_simpleblock ?
    BLOCK_BLOB_ALWAYS_SIMPLE : BLOCK_BLOB_NO_SIMPLE;

  assert((clstr != NULL) && !clstr->rendered);

  max_cl_timecode = 0;
  cluster = clstr->cluster;

  // Splitpoint stuff
  if ((header_overhead == -1) && splitting())
    header_overhead = out->getFilePointer() + tags_size;

  elements_in_cluster = 0;
  last_block_group = NULL;
  added_to_cues = false;

  if (hack_engaged(ENGAGE_LACING_XIPH))
    lacing_type = LACING_XIPH;
  else if (hack_engaged(ENGAGE_LACING_EBML))
    lacing_type = LACING_EBML;
  else
    lacing_type = LACING_AUTO;

  for (i = 0; i < clstr->packets.size(); i++) {
    pack = clstr->packets[i];
    source = pack->source;

    has_codec_state = NULL != pack->codec_state.get();

    if (source->contains_gap())
      cluster->SetSilentTrackUsed();

    render_group = NULL;
    for (k = 0; k < render_groups.size(); k++)
      if (render_groups[k]->source == source) {
        render_group = render_groups[k];
        break;
      }
    if (NULL == render_group) {
      render_group = new render_groups_t;
      render_group->source = source;
      render_group->more_data = false;
      render_group->duration_mandatory = false;
      render_groups.push_back(render_group);
    }

    if (i == 0)
      static_cast<kax_cluster_c *>
        (cluster)->set_min_timecode(pack->assigned_timecode - timecode_offset);
    max_cl_timecode = pack->assigned_timecode;

    data_buffer = new DataBuffer((binary *)pack->data->get(),
                                 pack->data->get_size());

    KaxTrackEntry &track_entry =
      static_cast<KaxTrackEntry &>(*source->get_track_entry());

    if (render_group->groups.size() > 0)
      last_block_group = render_group->groups.back();
    else
      last_block_group = NULL;

    if ((pack->bref != -1) || has_codec_state)
      render_group->more_data = false;

    if (!render_group->more_data) {
      set_duration(render_group);
      render_group->durations.clear();
      render_group->duration_mandatory = false;

      this_block_blob_type = !use_simpleblock ? std_block_blob_type :
        must_duration_be_set(render_group, pack) ?
        BLOCK_BLOB_NO_SIMPLE : BLOCK_BLOB_ALWAYS_SIMPLE;

      if (has_codec_state)
        this_block_blob_type = BLOCK_BLOB_NO_SIMPLE;

      new_block_group = new kax_block_blob_c(this_block_blob_type);
      cluster->AddBlockBlob(new_block_group);
      new_block_group->SetParent(*cluster);
      render_group->groups.push_back(new_block_group);
      added_to_cues = false;
    } else
      new_block_group = last_block_group;

    // Now put the packet into the cluster.
    render_group->more_data =
      new_block_group->add_frame_auto(track_entry,
                                      pack->assigned_timecode -
                                      timecode_offset,
                                      *data_buffer, lacing_type,
                                      pack->bref - timecode_offset,
                                      pack->fref - timecode_offset);

    if ((-1 == pack->bref) && (-1 == pack->fref))
      free_ref(pack->timecode, pack->source);

    if (has_codec_state) {
      KaxBlockGroup &bgroup((KaxBlockGroup &)*new_block_group);
      KaxCodecState *cstate = new KaxCodecState;
      bgroup.PushElement(*cstate);
      cstate->CopyBuffer(pack->codec_state->get(),
                         pack->codec_state->get_size());
    }

    if (first_timecode_in_file == -1)
      first_timecode_in_file = pack->assigned_timecode;

    if ((pack->assigned_timecode + pack->duration) > max_timecode_and_duration)
      max_timecode_and_duration = pack->assigned_timecode + pack->duration;

    if ((pack->bref != -1) || (pack->fref != -1) ||
        !track_entry.LacingEnabled())
      render_group->more_data = false;

    render_group->durations.push_back(pack->unmodified_duration);
    render_group->duration_mandatory |= pack->duration_mandatory;

    if (new_block_group != NULL) {
      // Set the reference priority if it was wanted.
      if ((pack->ref_priority > 0) &&
          new_block_group->replace_simple_by_group())
        *static_cast<EbmlUInteger *>
          (&GetChild<KaxReferencePriority>(*new_block_group)) =
          pack->ref_priority;

      // Handle BlockAdditions if needed
      if (!pack->data_adds.empty() &&
          new_block_group->ReplaceSimpleByGroup()) {
        KaxBlockAdditions &additions =
          AddEmptyChild<KaxBlockAdditions>(*new_block_group);
        for (k = 0; k < pack->data_adds.size(); k++) {
          KaxBlockMore &block_more = AddEmptyChild<KaxBlockMore>(additions);
          *static_cast<EbmlUInteger *>(&GetChild<KaxBlockAddID>(block_more)) =
            k + 1;
          GetChild<KaxBlockAdditional>(block_more).
            CopyBuffer((binary *)pack->data_adds[k]->get(),
                       pack->data_adds[k]->get_size());
        }
      }
    }

    elements_in_cluster++;

    if (new_block_group == NULL)
      new_block_group = last_block_group;
    else if (write_cues && (!added_to_cues || has_codec_state)) {
      // Update the cues (index table) either if cue entries for
      // I frames were requested and this is an I frame...
      if (((source->get_cue_creation() == CUE_STRATEGY_IFRAMES) &&
           (pack->bref == -1)) ||
          // ... or if a codec state change is present ...
          has_codec_state ||
          // ... or if the user requested entries for all frames ...
          (source->get_cue_creation() == CUE_STRATEGY_ALL) ||
          // ... or if this is an audio track, there is no video track and the
          // last cue entry was created more than 2s ago.
          ((source->get_cue_creation() == CUE_STRATEGY_SPARSE) &&
           (source->get_track_type() == track_audio) &&
           (video_packetizer == NULL) &&
           ((source->get_last_cue_timecode() < 0) ||
            ((pack->assigned_timecode - source->get_last_cue_timecode()) >=
             2000000000)))) {
        kax_cues->AddBlockBlob(*new_block_group);
        num_cue_elements++;
        cue_writing_requested = 1;
        source->set_last_cue_timecode(pack->assigned_timecode);
        added_to_cues = true;

        if (has_codec_state) {
        }
      }
    }

    pack->group = new_block_group;
    last_block_group = new_block_group;

  }

  if (elements_in_cluster > 0) {
    for (i = 0; i < render_groups.size(); i++)
      set_duration(render_groups[i]);
    static_cast<kax_cluster_c *>(cluster)->
      set_max_timecode(max_cl_timecode - timecode_offset);
    cluster->Render(*out, *kax_cues);
    bytes_in_file += cluster->ElementSize();

    if (kax_sh_cues != NULL)
      kax_sh_cues->IndexThis(*cluster, *kax_segment);

    last_cluster_tc = cluster->GlobalTimecode();
  } else
    last_cluster_tc = 0;

  for (i = 0; i < clstr->packets.size(); i++)
    clstr->packets[i]->data = memory_cptr(NULL);

  for (i = 0; i < render_groups.size(); i++)
    delete render_groups[i];

  clstr->rendered = true;

  free_clusters();

  min_timecode_in_cluster = -1;
  max_timecode_in_cluster = -1;

  return 1;
}

ch_contents_t *
cluster_helper_c::find_packet_cluster(int64_t ref_timecode,
                                      generic_packetizer_c *source) {
  int i, k;
  int64_t tolerance;

  if (clusters.size() == 0)
    return NULL;

  tolerance = source->reader->reference_timecode_tolerance;
  if (0 == tolerance)
    tolerance = 10000;

  // Be a bit fuzzy and allow timecodes that are 10us off.
  for (i = 0; i < clusters.size(); i++)
    for (k = 0; k < clusters[i]->packets.size(); k++) {
      packet_cptr &pack = clusters[i]->packets[k];
      if ((pack->source == source) &&
          (iabs(pack->timecode - ref_timecode) <= tolerance))
        return clusters[i];
    }

  return NULL;
}

packet_cptr
cluster_helper_c::find_packet(int64_t ref_timecode,
                              generic_packetizer_c *source) {
  int i, k;
  int64_t tolerance;

  if (clusters.size() == 0)
    return packet_cptr(NULL);

  tolerance = source->reader->reference_timecode_tolerance;
  if (0 == tolerance)
    tolerance = 10000;

  // Be a bit fuzzy and allow timecodes that are 10us off.
  for (i = 0; i < clusters.size(); i++)
    for (k = 0; k < clusters[i]->packets.size(); k++) {
      packet_cptr &pack = clusters[i]->packets[k];
      if ((pack->source == source) &&
          (iabs(pack->timecode - ref_timecode) <= tolerance))
        return pack;
    }

  return packet_cptr(NULL);
}

void
cluster_helper_c::check_clusters(int num) {
  int i, k;
  ch_contents_t *clstr;

  for (i = 0; i < clusters.size(); i++) {
    for (k = 0; k < clusters[i]->packets.size(); k++) {
      packet_cptr &p = clusters[i]->packets[k];
      if (clusters[i]->rendered && p->superseeded)
        continue;
      if (p->bref == -1)
        continue;
      clstr = find_packet_cluster(p->bref, p->source);
      if (clstr == NULL)
        die("cluster_helper.cpp/cluster_helper_c::check_clusters(): Error: "
            "backward refenrece could not be resolved (" LLD " -> " LLD "). "
            "Called from line %d.\n", p->timecode, p->bref, num);
    }
  }
}

// #define PRINT_CLUSTERS

int
cluster_helper_c::free_clusters() {
  int i, k;
  ch_contents_t *clstr;
#ifdef PRINT_CLUSTERS
  int num_freed = 0;
#endif

  if (clusters.size() == 0)
    return 0;

  for (i = 0; i < clusters.size(); i++)
    clusters[i]->is_referenced = false;

  // Part 1 - Mark all packets superseeded for which their source has
  // an appropriate free_refs entry.
  for (i = 0; i < clusters.size(); i++) {
    for (k = 0; k < clusters[i]->packets.size(); k++) {
      packet_cptr &p = clusters[i]->packets[k];
      if (p->source->get_free_refs() > p->timecode)
        p->superseeded = true;
    }
  }

  // Part 2 - Mark all clusters that are still referenced.
  for (i = 0; i < clusters.size(); i++) {
    for (k = 0; k < clusters[i]->packets.size(); k++) {
      packet_cptr &p = clusters[i]->packets[k];
      if (!p->superseeded) {
        clusters[i]->is_referenced = true;
        if (p->bref == -1)
          continue;
        clstr = find_packet_cluster(p->bref, p->source);
        if (clstr == NULL)
          die("cluster_helper.cpp/cluster_helper_c::free_clusters(): Error: "
              "backward refenrece could not be resolved (" LLD ").\n",
              p->bref);
        clstr->is_referenced = true;
      }
    }
  }

  // Part 3 - remove all clusters and the data belonging to them that
  // are not referenced anymore and that have already been rendered.
  // Also count the number of clusters that are still referenced.
  k = 0;
  for (i = 0; i < clusters.size(); i++) {
    if (!clusters[i]->rendered) {
      k++;
      continue;
    }

    if (!clusters[i]->is_referenced) {
      delete clusters[i];
      clusters[i] = NULL;
#ifdef PRINT_CLUSTERS
      num_freed++;
#endif
    } else
      k++;
  }

  // Part 4 - prune the cluster list and remove all the entries freed in
  // part 3.
  if (k == 0) {
    clusters.clear();
    add_cluster(new kax_cluster_c());
  } else if (k != clusters.size()) {
    vector<ch_contents_t *> new_clusters;

    for (i = 0; i < clusters.size(); i++)
      if (clusters[i] != NULL)
        new_clusters.push_back(clusters[i]);

    clusters = new_clusters;
  }

#ifdef PRINT_CLUSTERS
  mxdebug("numcl: %8d freed: %3d ", clusters.size(), num_freed);
  for (i = 0; i < clusters.size(); i++)
    mxinfo("#");
  mxinfo("\n");
#endif

  return 1;
}

int
cluster_helper_c::free_ref(int64_t ref_timecode,
                           generic_packetizer_c *source) {
  source->set_free_refs(ref_timecode);

  return 1;
}

int64_t
cluster_helper_c::get_duration() {
  mxverb(3, "cluster_helper_c::get_duration(): " LLD " - " LLD " = " LLD "\n",
         max_timecode_and_duration, first_timecode_in_file,
         max_timecode_and_duration - first_timecode_in_file);
  return max_timecode_and_duration - first_timecode_in_file;
}

void
cluster_helper_c::add_split_point(const split_point_t &split_point) {
  split_points.push_back(split_point);
  current_split_point = split_points.begin();
}
