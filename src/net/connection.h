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

  // How much the client has not taken yet. This is what the backpressure
  // watermarks are measured against, and it is exposed because a test driving a
  // real socket cannot arrange for an exact residual -- how much send() accepts
  // is the kernel's business -- but it can observe one.
  std::size_t pendingWriteBytes() const;

  // Whether this connection has been stopped for backpressure, as opposed to
  // stopped for good by QUIT or a protocol error.
  bool readPaused() const { return read_paused_; }

  // Whether this connection still has anything to read. False for two separate
  // reasons, and they are not the same kind of thing.
  //
  // A terminal slot ends reading for good: not replying is not enough on its
  // own, because under level-triggered epoll an unread readable socket reports
  // itself ready every time round and the loop spins.
  //
  // Backpressure ends it temporarily: above the high watermark the socket stops
  // being read from, so the kernel receive buffer fills and TCP flow control
  // pushes back on the sender, until the write buffer drains to the low one.
  bool wantsRead() const { return !stop_reading_ && !read_paused_; }

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

  // Decides whether this connection should be read from, from what the client
  // has not taken yet. Called from flush() and nowhere else: flush() is not the
  // only thing that changes the write buffer -- takeReadyPrefix() appends to it
  // first -- but it is where every path that changes it comes to rest.
  void updateBackpressure();

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
  // Backpressure: set when the write buffer reaches the high watermark, cleared
  // when it falls to the low one. Owned by this connection and touched only by
  // its own loop's thread -- it must not become the first piece of mutable
  // state two threads can reach.
  bool read_paused_ = false;
  std::uint32_t registered_events_ = 0;
};

}  // namespace shardkv
