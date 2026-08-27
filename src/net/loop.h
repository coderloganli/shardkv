#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "base/unique_fd.h"
#include "commands/dispatch.h"
#include "store/shard.h"

namespace shardkv {

class Connection;
class Listener;

// One epoll instance, one set of connections, one shard -- and, from the second
// task onward, one thread and one core.
//
// This is already the final shape. The first task constructs exactly one; the
// next constructs N, adds the cross-shard queue and the eventfd, and does not
// have to take this apart to do it. There is no synchronisation primitive
// anywhere inside, now or later: a Loop is touched by its own thread only.
class Loop {
 public:
  Loop(std::uint16_t port, Shard& shard);
  ~Loop();

  Loop(const Loop&) = delete;
  Loop& operator=(const Loop&) = delete;

  // The bound port. When constructed with 0 the kernel chooses one, which is
  // what the integration tests use so they do not collide.
  std::uint16_t port() const;

  // Runs until stop(). Level-triggered: EPOLLOUT is registered only when a
  // write comes up short and dropped as soon as the buffer drains.
  void run();

  // Safe to call from another thread; wakes the loop.
  void stop();

 private:
  void acceptReady();
  void closeConnection(int fd);

  // Adds or drops EPOLLOUT to match whether the connection has pending bytes.
  void updateInterest(Connection& connection);

  // epoll_event.data carries the fd, never a pointer. Two reasons, both real:
  // one batch from epoll_wait can contain an event for a connection that an
  // earlier event in the same batch closed, so a pointer would be a
  // use-after-free; and fd numbers are reused by the kernel, so a lookup that
  // misses is ignored rather than asserted on.
  //
  // Teardown order is EPOLL_CTL_DEL, then erase from this map -- the reverse
  // leaves a closed fd registered in the epoll set.
  std::unordered_map<int, std::unique_ptr<Connection>> connections_;

  UniqueFd epoll_fd_;
  std::unique_ptr<Listener> listener_;
  Shard* shard_ = nullptr;
  UniqueFd wake_fd_;
  std::uint16_t port_ = 0;

  // Kept in step with connections_ so INFO reports the live count.
  LoopStats stats_;

  // Written by stop() from another thread, read by run().
  std::atomic<bool> stopping_{false};
};

}  // namespace shardkv
