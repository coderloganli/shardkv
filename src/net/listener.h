#pragma once

#include <cstdint>

#include "base/unique_fd.h"

namespace shardkv {

// A listening socket with SO_REUSEPORT set.
//
// With one loop the option changes nothing observable. It is set now anyway,
// because the next task gives every loop its own listener on the same port and
// lets the kernel hash incoming connections across them -- and this file should
// not have to change for that.
class Listener {
 public:
  // port 0 asks the kernel to choose; read it back with port().
  explicit Listener(std::uint16_t port);

  int fd() const;
  std::uint16_t port() const;

  // Accepts one pending connection, already set non-blocking. Returns an empty
  // UniqueFd when there is nothing to accept.
  UniqueFd accept();

 private:
  UniqueFd fd_;
  std::uint16_t port_ = 0;
};

}  // namespace shardkv
