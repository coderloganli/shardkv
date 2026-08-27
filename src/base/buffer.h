#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace shardkv {

// A connection's read or write buffer, reused across requests rather than
// reallocated per request.
//
// v1 only ever grows. Compacting the consumed prefix arrives with the rest of
// the resource management, so until then a long-lived connection holds a buffer
// as large as its largest burst. Known and accepted; see docs/architecture.md.
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

 private:
  std::string data_;
  std::size_t start_ = 0;
};

}  // namespace shardkv
