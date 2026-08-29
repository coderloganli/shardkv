#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace shardkv {

// A connection's read or write buffer, reused across requests rather than
// reallocated per request.
//
// The consumed prefix is reclaimed; the capacity is not. A buffer resets when
// it drains completely and otherwise compacts once the consumed prefix is both
// large in absolute terms and at least half the buffer -- so it no longer grows
// with the NUMBER of requests a connection has served, which had no bound. It
// does still sit at the size of the BIGGEST one, which does: that allocation is
// kept and reused deliberately. See
// docs/adr/0011-buffers-compact-their-consumed-prefix-but-keep-their-capacity.md
class Buffer {
 public:
  // The bytes not yet consumed. Invalidated by append() and consume() -- which
  // is the same invalidation the parser's argv views inherit.
  std::string_view readable() const;

  void append(std::string_view bytes);

  // Drops n bytes from the front.
  void consume(std::size_t n);

  std::size_t size() const;
  bool empty() const;

  // What the buffer has allocated, unread and consumed alike. Exposed so that
  // a test can pin both halves of what compaction does -- that the buffer stops
  // growing, and that it does not start giving memory back.
  std::size_t capacity() const;

 private:
  std::string data_;
  std::size_t start_ = 0;
};

}  // namespace shardkv
