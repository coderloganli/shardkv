#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "base/buffer.h"
#include "base/unique_fd.h"
#include "commands/dispatch.h"
#include "commands/router.h"
#include "net/reply_slots.h"
#include "store/shard.h"

namespace shardkv {

// One client. Owns its two buffers and the vector the parser fills, all reused
// for the life of the connection.
class Connection {
 public:
  // router and stats are owned by the loop and outlive every connection on it.
  Connection(UniqueFd fd, ShardRouter& router, LoopStats& stats, std::uint64_t id);

  std::uint64_t id() const { return id_; }

  // A cross-shard reply has come back for this connection. Fills its slot and
  // writes out whatever is now ready.
  ReplySlots& slots() { return slots_; }
  bool onSlotsChanged();

  int fd() const;

  // Reads what is available and answers every whole command in it. Returns
  // false when the connection should be closed -- peer gone, QUIT, or a
  // protocol error, which is unrecoverable because the stream can no longer be
  // resynchronised.
  bool onReadable();

  // Sends what is pending. Returns false on a fatal error.
  bool onWritable();

  // Whether a short write left something behind, i.e. whether EPOLLOUT should
  // be registered.
  bool wantsWrite() const;

  // Whether this connection still has anything to read. False once a terminal
  // slot exists: not reading is not enough on its own, because under
  // level-triggered epoll an unread readable socket reports itself ready every
  // time round and the loop spins.
  bool wantsRead() const { return !stop_reading_; }

  // What the loop last told epoll about this connection, so that it only issues
  // EPOLL_CTL_MOD when the answer actually changed.
  std::uint32_t registeredEvents() const { return registered_events_; }
  void setRegisteredEvents(std::uint32_t events) { registered_events_ = events; }

 private:
  // Fixed order, and not a matter of taste: the parser's views point into
  // read_, so everything that must outlive the call is copied during dispatch,
  // and only then are the bytes consumed. See proto/parser.h.
  bool drainInput();

  // Non-blocking send with MSG_NOSIGNAL -- writing to a closed peer otherwise
  // raises SIGPIPE, whose default disposition kills the process. EINTR retries,
  // EAGAIN is treated as a short write, EPIPE and ECONNRESET close quietly.
  bool flush();

  UniqueFd fd_;
  ShardRouter* router_ = nullptr;
  LoopStats* stats_ = nullptr;
  std::uint64_t id_ = 0;
  ReplySlots slots_;
  Buffer read_;
  Buffer write_;
  std::vector<std::string_view> argv_;
  // Set by QUIT or a protocol error. Both reserve a terminal slot rather than
  // replying at once, so they cannot overtake replies the client is still owed;
  // the connection closes when that slot has been flushed in its turn.
  bool close_after_flush_ = false;
  // Once a terminal slot exists, no further command is parsed from this
  // connection: whatever follows QUIT or a broken frame is not ours to answer.
  bool stop_reading_ = false;
  std::uint32_t registered_events_ = 0;
};

}  // namespace shardkv
