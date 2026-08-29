#pragma once

#include <cstdint>

#include "base/unique_fd.h"

namespace shardkv {

// A listening socket with SO_REUSEPORT set.
//
// Every loop has one of these on the same port, and the kernel hands each
// incoming connection to exactly one of them. How it chooses is not documented:
// socket(7) promises improved accept distribution, not an algorithm, and the
// assignment can be redefined outright with a BPF program. So nothing here
// depends on the spread being even. What is relied on is only that a connection
// arrives at one loop and stays there.
class Listener {
 public:
  // port 0 asks the kernel to choose; read it back with port().
  explicit Listener(std::uint16_t port);

  int fd() const;
  std::uint16_t port() const;

  // Accepts one pending connection, already set non-blocking. Returns an empty
  // UniqueFd when there is nothing to accept.
  // Takes one pending connection, or returns an empty descriptor.
  //
  // `out_of_descriptors`, when given, distinguishes the two ways that can
  // happen: an empty backlog leaves it false, while EMFILE or ENFILE -- a
  // connection waiting that could not be given a descriptor -- sets it. The
  // caller must tell them apart, because under level-triggered epoll retrying
  // the second is a spin that never ends.
  UniqueFd accept(bool* out_of_descriptors = nullptr);

 private:
  UniqueFd fd_;
  std::uint16_t port_ = 0;
};

}  // namespace shardkv
