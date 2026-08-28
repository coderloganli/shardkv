#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "base/mpsc_queue.h"
#include "base/unique_fd.h"
#include "commands/dispatch.h"
#include "commands/router.h"
#include "net/message.h"
#include "base/buffer.h"
#include "store/shard.h"

namespace shardkv {

class Connection;
class Listener;
class Loop;

// A loop's two inboxes and the eventfd that wakes it. Held outside the Loop so
// that every loop can reach every other's, without any loop reaching another's
// shard, connections or slots.
struct Inbox {
  MpscQueue<CrossShardRequest> requests;
  MpscQueue<CrossShardReply> replies;
  int wake_fd = -1;
};

// Shared by construction and by nothing else: the queues are the only channel
// between threads, and `stopping` is the flag that lets every producer stop
// before any consumer is torn down.
struct LoopTable {
  std::vector<std::unique_ptr<Inbox>> inboxes;
  std::atomic<bool> stopping{false};
};

// One epoll instance, one set of connections, one shard -- and, from the second
// task onward, one thread and one core.
//
// This is already the final shape. The first task constructs exactly one; the
// next constructs N, adds the cross-shard queue and the eventfd, and does not
// have to take this apart to do it. There is no synchronisation primitive
// anywhere inside, now or later: a Loop is touched by its own thread only.
class Loop {
 public:
  // `inboxes` is the table of every loop's queues, this one included, so a
  // loop can deliver to any other. Nothing in it is written by more than one
  // thread except through the queues themselves.
  Loop(std::uint16_t port, Shard& shard, std::size_t index, LoopTable& table,
       bool pin);
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

  // Takes everything queued for this loop: requests from other loops to run
  // against this shard, and replies to commands this loop sent out.
  void drainInbox();

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
  std::size_t index_ = 0;
  bool pin_requested_ = false;
  LoopTable* table_ = nullptr;
  std::unique_ptr<ShardRouter> router_;
  LoopStats stats_;
  std::uint64_t next_conn_id_ = 0;
  UniqueFd wake_fd_;
  std::uint16_t port_ = 0;


  // Written by stop() from another thread, read by run().
  std::atomic<bool> stopping_{false};
};

}  // namespace shardkv
