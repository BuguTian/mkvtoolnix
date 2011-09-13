/*
   mkvmerge -- utility for splicing together matroska files
   from component media subtypes

   Distributed under the GPL
   see the file COPYING for details
   or visit http://www.gnu.org/copyleft/gpl.html

   class definition for memory handling classes

   Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#ifndef __MTX_COMMON_MEMORY_H
#define __MTX_COMMON_MEMORY_H

#include <cassert>
#include <deque>

#include "common/common_pch.h"

inline void
safefree(void *p) {
  if (NULL != p)
    free(p);
}

#define safemalloc(s) _safemalloc(s, __FILE__, __LINE__)
unsigned char *MTX_DLL_API _safemalloc(size_t size, const char *file, int line);

#define safememdup(src, size) _safememdup(src, size, __FILE__, __LINE__)
unsigned char *MTX_DLL_API _safememdup(const void *src, size_t size, const char *file, int line);

#define safestrdup(s) _safestrdup(s, __FILE__, __LINE__)
inline char *
_safestrdup(const std::string &s,
            const char *file,
            int line) {
  return reinterpret_cast<char *>(_safememdup(s.c_str(), s.length() + 1, file, line));
}
inline unsigned char *
_safestrdup(const unsigned char *s,
            const char *file,
            int line) {
  return _safememdup(s, strlen(reinterpret_cast<const char *>(s)) + 1, file, line);
}
inline char *
_safestrdup(const char *s,
            const char *file,
            int line) {
  return reinterpret_cast<char *>(_safememdup(s, strlen(s) + 1, file, line));
}

#define saferealloc(mem, size) _saferealloc(mem, size, __FILE__, __LINE__)
unsigned char *MTX_DLL_API _saferealloc(void *mem, size_t size, const char *file, int line);

class MTX_DLL_API memory_c;
typedef counted_ptr<memory_c> memory_cptr;
typedef std::vector<memory_cptr> memories_c;

class MTX_DLL_API memory_c {
public:
  typedef unsigned char X;

  explicit memory_c(void *p = NULL,
                    size_t s = 0,
                    bool f = false) // allocate a new counter
    : its_counter(NULL)
  {
    if (p)
      its_counter = new counter(static_cast<unsigned char *>(p), s, f);
  }

  ~memory_c() {
    release();
  }

  memory_c(const memory_c &r) throw() {
    acquire(r.its_counter);
  }

  memory_c &operator=(const memory_c &r) {
    if (this != &r) {
      release();
      acquire(r.its_counter);
    }
    return *this;
  }

  X *get_buffer() const throw() {
    return its_counter ? its_counter->ptr + its_counter->offset : NULL;
  }

  size_t get_size() const throw() {
    return its_counter ? its_counter->size - its_counter->offset: 0;
  }

  void set_size(size_t new_size) throw() {
    if (its_counter)
      its_counter->size = new_size;
  }

  void set_offset(size_t new_offset) {
    if (!its_counter || (new_offset > its_counter->size))
      throw false;
    its_counter->offset = new_offset;
  }

  bool is_unique() const throw() {
    return (its_counter ? its_counter->count == 1 : true);
  }

  bool is_allocated() const throw() {
    return (NULL != its_counter) && (NULL != its_counter->ptr);
  }

  memory_c *clone() const {
    return new memory_c(static_cast<unsigned char *>(safememdup(get_buffer(), get_size())), get_size(), true);
  }

  bool is_free() const {
    return its_counter && its_counter->is_free;
  }

  void grab() {
    if (!its_counter || its_counter->is_free)
      return;

    its_counter->ptr      = static_cast<unsigned char *>(safememdup(get_buffer(), get_size()));
    its_counter->is_free  = true;
    its_counter->size    -= its_counter->offset;
    its_counter->offset   = 0;
  }

  void lock() {
    if (its_counter)
      its_counter->is_free = false;
  }

  void resize(size_t new_size) throw();

  operator const unsigned char *() const {
    return its_counter ? its_counter->ptr : NULL;
  }

  operator const void *() const {
    return its_counter ? its_counter->ptr : NULL;
  }

  operator unsigned char *() const {
    return its_counter ? its_counter->ptr : NULL;
  }

  operator void *() const {
    return its_counter ? its_counter->ptr : NULL;
  }

public:
  static memory_cptr alloc(size_t size) {
    return memory_cptr(new memory_c(static_cast<unsigned char *>(safemalloc(size)), size, true));
  };

private:
  struct counter {
    X *ptr;
    size_t size;
    bool is_free;
    unsigned count;
    size_t offset;

    counter(X *p = NULL,
            size_t s = 0,
            bool f = false,
            unsigned c = 1)
      : ptr(p)
      , size(s)
      , is_free(f)
      , count(c)
      , offset(0)
    { }
  } *its_counter;

  void acquire(counter *c) throw() { // increment the count
    its_counter = c;
    if (c)
      ++c->count;
  }

  void release() { // decrement the count, delete if it is 0
    if (its_counter) {
      if (--its_counter->count == 0) {
        if (its_counter->is_free)
          free(its_counter->ptr);
        delete its_counter;
      }
      its_counter = 0;
    }
  }
};

class MTX_DLL_API memory_slice_cursor_c {
 protected:
  size_t m_pos, m_pos_in_slice, m_size;
  std::deque<memory_cptr> m_slices;
  std::deque<memory_cptr>::iterator m_slice;

 public:
  memory_slice_cursor_c()
    : m_pos(0)
    , m_pos_in_slice(0)
    , m_size(0)
    , m_slice(m_slices.end())
  {
  }

  memory_slice_cursor_c(const memory_slice_cursor_c &) {
    mxerror(Y("memory_slice_cursor_c copy c'tor: Must not be used!"));
  }

  ~memory_slice_cursor_c() {
  }

  void add_slice(memory_cptr slice) {
    if (slice->get_size() == 0)
      return;

    if (m_slices.end() == m_slice) {
      m_slices.push_back(slice);
      m_slice = m_slices.begin();

    } else {
      size_t pos = std::distance(m_slices.begin(), m_slice);
      m_slices.push_back(slice);
      m_slice = m_slices.begin() + pos;
    }

    m_size += slice->get_size();
  }

  void add_slice(unsigned char *buffer, size_t size) {
    if (0 == size)
      return;

    add_slice(memory_cptr(new memory_c(buffer, size, false)));
  }

  inline unsigned char get_char() {
    assert(m_pos < m_size);

    memory_c &slice = *(*m_slice).get_object();
    unsigned char c = *(slice.get_buffer() + m_pos_in_slice);

    ++m_pos_in_slice;
    ++m_pos;

    if (m_pos_in_slice < slice.get_size())
      return c;

    ++m_slice;
    m_pos_in_slice = 0;

    return c;
  };

  inline bool char_available() {
    return m_pos < m_size;
  };

  inline size_t get_remaining_size() {
    return m_size - m_pos;
  };

  inline size_t get_size() {
    return m_size;
  };

  inline size_t get_position() {
    return m_pos;
  };

  void reset(bool clear_slices = false) {
    if (clear_slices) {
      m_slices.clear();
      m_size = 0;
    }
    m_pos          = 0;
    m_pos_in_slice = 0;
    m_slice        = m_slices.begin();
  };

  void copy(unsigned char *dest, size_t start, size_t size) {
    assert((start + size) <= m_size);

    std::deque<memory_cptr>::iterator curr = m_slices.begin();
    int offset = 0;

    while (start > ((*curr)->get_size() + offset)) {
      offset += (*curr)->get_size();
      curr++;
      assert(m_slices.end() != curr);
    }
    offset = start - offset;

    while (0 < size) {
      size_t num_bytes = (*curr)->get_size() - offset;
      if (num_bytes > size)
        num_bytes = size;

      memcpy(dest, (*curr)->get_buffer() + offset, num_bytes);

      size -= num_bytes;
      dest += num_bytes;
      curr++;
      offset = 0;
    }
  };

};

inline memory_cptr
clone_memory(const void *buffer,
             size_t size) {
  return memory_cptr(new memory_c(static_cast<unsigned char *>(safememdup(buffer, size)), size, true));
}

inline memory_cptr
clone_memory(memory_cptr data) {
  return clone_memory(data->get_buffer(), data->get_size());
}

struct buffer_t {
  unsigned char *m_buffer;
  size_t m_size;

  buffer_t();
  buffer_t(unsigned char *buffer, size_t m_size);
  ~buffer_t();
};

memory_cptr MTX_DLL_API lace_memory_xiph(const std::vector<memory_cptr> &blocks);
std::vector<memory_cptr> MTX_DLL_API unlace_memory_xiph(memory_cptr &buffer);

#endif  // __MTX_COMMON_MEMORY_H
