#pragma once

#include <string_view>
#include <vector>

#include "base/buffer.h"
#include "base/unique_fd.h"
#include "commands/dispatch.h"
#include "store/shard.h"

namespace shardkv {

// One client. Owns its two buffers and the vector the parser fills, all reused
// for the life of the connection.
class Connection {
 public:
  // stats is owned by the loop and outlives every connection on it.
  Connection(UniqueFd fd, Shard& shard, LoopStats& stats);

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

  // What the loop last told epoll about this connection, so that it only issues
  // EPOLL_CTL_MOD when the answer actually changed.
  bool registeredForWrite() const { return registered_for_write_; }
  void setRegisteredForWrite(bool value) { registered_for_write_ = value; }

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
  Shard* shard_ = nullptr;
  LoopStats* stats_ = nullptr;
  Buffer read_;
  Buffer write_;
  std::vector<std::string_view> argv_;
  bool close_after_flush_ = false;
  bool registered_for_write_ = false;
};

}  // namespace shardkv
