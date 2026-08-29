#include "base/buffer.h"

namespace shardkv {
namespace {

// Below this, compaction would move bytes to reclaim very little, and the
// request-at-a-time path -- which drains completely and never gets here -- would
// start paying for a burst behaviour it does not have.
constexpr std::size_t kCompactMinPrefix = 8 * 1024;

}  // namespace

std::string_view Buffer::readable() const {
  return std::string_view(data_).substr(start_);
}

void Buffer::append(std::string_view bytes) { data_.append(bytes); }

void Buffer::consume(std::size_t n) {
  start_ += n;

  if (start_ >= data_.size()) {
    // Fully drained: the cheap reset, and still the common case. Clearing an
    // empty buffer costs nothing.
    data_.clear();
    start_ = 0;
    return;
  }

  // Compaction, on two conditions, and both are load-bearing.
  //
  // The byte floor keeps a connection that consumes a few dozen bytes at a time
  // out of here entirely. The half condition is what bounds the cost: a
  // compaction moves no more bytes than it discards, so a byte is moved a
  // bounded number of times over its life in the buffer, whatever the traffic
  // pattern. On the floor alone, a large unread tail behind a barely-qualifying
  // prefix would be moved again and again to reclaim very little.
  //
  // erase() does not return the capacity, which is the point -- see the
  // decision record. This moves the tail down; it does not give memory back.
  if (start_ >= kCompactMinPrefix && start_ * 2 >= data_.size()) {
    data_.erase(0, start_);
    start_ = 0;
  }
}

std::size_t Buffer::size() const { return data_.size() - start_; }

bool Buffer::empty() const { return size() == 0; }

std::size_t Buffer::capacity() const { return data_.capacity(); }

}  // namespace shardkv
