#include "base/buffer.h"

namespace shardkv {

std::string_view Buffer::readable() const {
  return std::string_view(data_).substr(start_);
}

void Buffer::append(std::string_view bytes) { data_.append(bytes); }

void Buffer::consume(std::size_t n) {
  start_ += n;
  if (start_ >= data_.size()) {
    // Fully drained: the cheap reset. This is not the compaction that the
    // resource-management task adds -- that one reclaims a large consumed
    // prefix while unread bytes remain behind it. Clearing an empty buffer
    // costs nothing and keeps the common request-at-a-time case from growing
    // without bound.
    data_.clear();
    start_ = 0;
  }
}

std::size_t Buffer::size() const { return data_.size() - start_; }

bool Buffer::empty() const { return size() == 0; }

}  // namespace shardkv
