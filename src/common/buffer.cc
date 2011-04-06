// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#include "armor.h"
#include "common/debug.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include "common/config.h"
#include "include/Spinlock.h"
#include "include/types.h"

#include <errno.h>
#include <fstream>
#include <sstream>
#include <sys/uio.h>
#include <limits.h>

namespace ceph {

Spinlock buffer_lock("buffer_lock");
atomic_t buffer_total_alloc;
bool buffer_track_alloc = true;

  void buffer::inc_total_alloc(unsigned len) {
    if (buffer_track_alloc)
      buffer_total_alloc.add(len);
  }
  void buffer::dec_total_alloc(unsigned len) {
    if (buffer_track_alloc)
      buffer_total_alloc.sub(len);
  }
  int buffer::get_total_alloc() {
    return buffer_total_alloc.read();
  }

  buffer::raw* buffer::copy(const char *c, unsigned len) {
    raw* r = new raw_char(len);
    memcpy(r->data, c, len);
    return r;
  }
  buffer::raw* buffer::create(unsigned len) {
    return new raw_char(len);
  }
  buffer::raw* buffer::claim_char(unsigned len, char *buf) {
    return new raw_char(len, buf);
  }
  buffer::raw* buffer::create_malloc(unsigned len) {
    return new raw_malloc(len);
  }
  buffer::raw* buffer::claim_malloc(unsigned len, char *buf) {
    return new raw_malloc(len, buf);
  }
  buffer::raw* buffer::create_static(unsigned len, char *buf) {
    return new raw_static(buf, len);
  }
  buffer::raw* buffer::create_page_aligned(unsigned len) {
#ifndef __CYGWIN__
    //return new raw_mmap_pages(len);
    return new raw_posix_aligned(len);
#else
    return new raw_hack_aligned(len);
#endif
  }

  buffer::ptr::ptr(raw *r) : _raw(r), _off(0), _len(r->len) {   // no lock needed; this is an unref raw.
    r->nref.inc();
    bdout << "ptr " << this << " get " << _raw << bendl;
  }
  buffer::ptr::ptr(unsigned l) : _off(0), _len(l) {
    _raw = create(l);
    _raw->nref.inc();
    bdout << "ptr " << this << " get " << _raw << bendl;
  }
  buffer::ptr::ptr(const char *d, unsigned l) : _off(0), _len(l) {    // ditto.
    _raw = copy(d, l);
    _raw->nref.inc();
    bdout << "ptr " << this << " get " << _raw << bendl;
  }
  buffer::ptr::ptr(const ptr& p) : _raw(p._raw), _off(p._off), _len(p._len) {
    if (_raw) {
      _raw->nref.inc();
      bdout << "ptr " << this << " get " << _raw << bendl;
    }
  }
  buffer::ptr::ptr(const ptr& p, unsigned o, unsigned l) : _raw(p._raw), _off(p._off + o), _len(l) {
    assert(o+l <= p._len);
    assert(_raw);
    _raw->nref.inc();
    bdout << "ptr " << this << " get " << _raw << bendl;
  }
  buffer::ptr& buffer::ptr::operator= (const ptr& p) {
    // be careful -- we need to properly handle self-assignment.
    if (p._raw) {
      p._raw->nref.inc();                      // inc new
      bdout << "ptr " << this << " get " << _raw << bendl;
    }
    release();                                 // dec (+ dealloc) old (if any)
    if (p._raw) {
      _raw = p._raw;
      _off = p._off;
      _len = p._len;
    } else {
      _off = _len = 0;
    }
    return *this;
  }

  buffer::raw *buffer::ptr::clone() {
    return _raw->clone();
  }

  void buffer::ptr::clone_in_place() {
    raw *newraw = _raw->clone();
    release();
    newraw->nref.inc();
    _raw = newraw;
  }
  bool buffer::ptr::do_cow() {
    if (_raw->nref.read() > 1) {
      //std::cout << "doing cow on " << _raw << " len " << _len << std::endl;
      clone_in_place();
      return true;
    } else
      return false;
  }

  void buffer::ptr::release() {
    if (_raw) {
      bdout << "ptr " << this << " release " << _raw << bendl;
      if (_raw->nref.dec() == 0) {
	//cout << "hosing raw " << (void*)_raw << " len " << _raw->len << std::endl;
	delete _raw;  // dealloc old (if any)
      }
      _raw = 0;
    }
  }

void buffer::list::encode_base64(buffer::list& o)
{
  bufferptr bp(length() * 4 / 3 + 3);
  int l = ceph_armor(bp.c_str(), bp.c_str() + bp.length(), c_str(), c_str() + length());
  bp.set_length(l);
  o.push_back(bp);
}

void buffer::list::decode_base64(buffer::list& e)
{
  bufferptr bp(4 + ((e.length() * 3) / 4));
  int l = ceph_unarmor(bp.c_str(), bp.c_str() + bp.length(), e.c_str(), e.c_str() + e.length());
  if (l < 0) {
    std::ostringstream oss;
    oss << "decode_base64: decoding failed:\n";
    hexdump(oss);
    throw buffer::malformed_input(oss.str().c_str());
  }
  assert(l <= (int)bp.length());
  bp.set_length(l);
  push_back(bp);
}

void buffer::list::rebuild_page_aligned()
{
  std::list<ptr>::iterator p = _buffers.begin();
  while (p != _buffers.end()) {
    // keep anything that's already page sized+aligned
    if (p->is_page_aligned() && p->is_n_page_sized()) {
      /*generic_dout(0) << " segment " << (void*)p->c_str()
		      << " offset " << ((unsigned long)p->c_str() & ~PAGE_MASK)
		      << " length " << p->length()
		      << " " << (p->length() & ~PAGE_MASK) << " ok" << dendl;
      */
      p++;
      continue;
    }
    
    // consolidate unaligned items, until we get something that is sized+aligned
    list unaligned;
    unsigned offset = 0;
    do {
      /*generic_dout(0) << " segment " << (void*)p->c_str()
		      << " offset " << ((unsigned long)p->c_str() & ~PAGE_MASK)
		      << " length " << p->length() << " " << (p->length() & ~PAGE_MASK)
		      << " overall offset " << offset << " " << (offset & ~PAGE_MASK)
		      << " not ok" << dendl;
      */
      offset += p->length();
      unaligned.push_back(*p);
      _buffers.erase(p++);
    } while (p != _buffers.end() &&
	     (!p->is_page_aligned() ||
	      !p->is_n_page_sized() ||
	      (offset & ~PAGE_MASK)));
    unaligned.rebuild();
    _buffers.insert(p, unaligned._buffers.front());
  }
}
  

int buffer::list::read_file(const char *fn, bool silent)
{
  int fd = TEMP_FAILURE_RETRY(::open(fn, O_RDONLY));
  if (fd < 0) {
    int err = errno;
    if (!silent) {
      derr << "can't open " << fn << ": " << cpp_strerror(err) << dendl;
    }
    return -err;
  }

  struct stat st;
  memset(&st, 0, sizeof(st));
  ::fstat(fd, &st);

  int s = ROUND_UP_TO(st.st_size, PAGE_SIZE);
  bufferptr bp = buffer::create_page_aligned(s);

  ssize_t ret = safe_read(fd, (void*)bp.c_str(), st.st_size);
  if (ret < 0) {
      if (!silent) {
	derr << "bufferlist::read_file(" << fn << "): read error:"
	     << cpp_strerror(ret) << dendl;
      }
      TEMP_FAILURE_RETRY(::close(fd));
      return ret;
  }
  else if (ret != st.st_size) {
      // Premature EOF.
      // Perhaps the file changed between stat() and read()?
      if (!silent) {
	derr << "bufferlist::read_file(" << fn << "): warning: got premature "
	     << "EOF:" << dendl;
      }
  }
  TEMP_FAILURE_RETRY(::close(fd));
  bp.set_length(ret);
  append(bp);
  return 0;
}

int buffer::list::write_file(const char *fn, int mode)
{
  int fd = TEMP_FAILURE_RETRY(::open(fn, O_WRONLY|O_CREAT|O_TRUNC, mode));
  if (fd < 0) {
    int err = errno;
    cerr << "bufferlist::write_file(" << fn << "): failed to open file: "
	 << cpp_strerror(err) << std::endl;
    return -err;
  }
  int ret = write_fd(fd);
  if (ret) {
    cerr << "bufferlist::write_fd(" << fn << "): write_fd error: "
	 << cpp_strerror(ret) << std::endl;
    TEMP_FAILURE_RETRY(::close(fd));
    return ret;
  }
  if (TEMP_FAILURE_RETRY(::close(fd))) {
    int err = errno;
    cerr << "bufferlist::write_file(" << fn << "): close error: "
	 << cpp_strerror(err) << std::endl;
    return -err;
  }
  return 0;
}

int buffer::list::write_fd(int fd) const
{
  // use writev!
  iovec iov[IOV_MAX];
  int iovlen = 0;
  ssize_t bytes = 0;

  std::list<ptr>::const_iterator p = _buffers.begin(); 
  while (p != _buffers.end()) {
    if (p->length() > 0) {
      iov[iovlen].iov_base = (void *)p->c_str();
      iov[iovlen].iov_len = p->length();
      bytes += p->length();
      iovlen++;
    }
    p++;

    if (iovlen == IOV_MAX-1 ||
	p == _buffers.end()) {
      iovec *start = iov;
      int num = iovlen;
      ssize_t wrote;
    retry:
      wrote = ::writev(fd, start, num);
      if (wrote < 0) {
	int err = errno;
	if (err == EINTR)
	  goto retry;
	return -err;
      }
      if (wrote < bytes) {
	// partial write, recover!
	while ((size_t)wrote >= start[0].iov_len) {
	  wrote -= start[0].iov_len;
	  bytes -= start[0].iov_len;
	  start++;
	  num--;
	}
	if (wrote > 0) {
	  start[0].iov_len -= wrote;
	  start[0].iov_base = (char *)start[0].iov_base + wrote;
	  bytes -= wrote;
	}
	goto retry;
      }
      iovlen = 0;
      bytes = 0;
    }
  }
  return 0;
}


void buffer::list::hexdump(std::ostream &out) const
{
  out.setf(std::ios::right);
  out.fill('0');

  unsigned per = 16;

  for (unsigned o=0; o<length(); o += per) {
    out << std::hex << std::setw(4) << o << " :";

    unsigned i;
    for (i=0; i<per && o+i<length(); i++) {
      out << " " << std::setw(2) << ((unsigned)(*this)[o+i] & 0xff);
    }
    for (; i<per; i++)
      out << "   ";
    
    out << " : ";
    for (i=0; i<per && o+i<length(); i++) {
      char c = (*this)[o+i];
      if (isupper(c) || islower(c) || isdigit(c) || c == ' ' || ispunct(c))
	out << c;
      else
	out << '.';
    }
    out << std::dec << std::endl;
  }
  out.unsetf(std::ios::right);
}

}
