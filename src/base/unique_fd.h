#pragma once

#include <unistd.h>

#include <utility>

namespace shardkv {

// Owns a file descriptor and closes it on destruction. Move-only: a descriptor
// has exactly one owner, so a double close is a compile error rather than
// something to remember.
//
// Raw close() does not appear anywhere else in this codebase. A descriptor leak
// is the failure that survives a one-hour soak unnoticed and then exhausts the
// process under load, so ownership is made structural.
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}

  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  // The descriptor, or -1 when this owns nothing.
  int get() const { return fd_; }

  // Gives up ownership without closing. The caller becomes responsible.
  int release() { return std::exchange(fd_, -1); }

  // Closes now, if anything is owned.
  void reset() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  explicit operator bool() const { return fd_ >= 0; }

 private:
  int fd_ = -1;
};

}  // namespace shardkv
