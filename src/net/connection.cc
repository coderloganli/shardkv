#include "net/connection.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <utility>

#include "commands/dispatch.h"
#include "proto/encoder.h"
#include "proto/parser.h"

namespace shardkv {
namespace {
constexpr std::size_t kReadChunk = 16 * 1024;

// Backpressure. Two watermarks rather than one, because a single threshold
// oscillates: a connection that drained one byte below it would be read from,
// climb straight back over, and pause again, at one EPOLL_CTL_MOD per byte.
//
// The gap is what turns that into one pause and one resume. See
// docs/adr/0010-backpressure-is-two-watermarks-and-never-a-disconnect.md
constexpr std::size_t kWriteHighWatermark = 1024 * 1024;
constexpr std::size_t kWriteLowWatermark = 256 * 1024;

// The invariant that keeps a paused connection from being registered for no
// events at all and never woken again: pausing needs bytes pending, and
// resuming happens while bytes are still pending, so a paused connection always
// wants EPOLLOUT. A low watermark at or above the high one would break it.
static_assert(kWriteLowWatermark < kWriteHighWatermark,
              "the low watermark must leave a paused connection something to write");
}  // namespace

Connection::Connection(UniqueFd fd, ShardRouter& router, LoopStats& stats,
                       std::uint64_t id)
    : fd_(std::move(fd)), router_(&router), stats_(&stats), id_(id) {}

int Connection::fd() const { return fd_.get(); }

bool Connection::onReadable() {
  // A terminal slot means this connection is finished: whatever the client
  // sends after QUIT or a broken frame is not ours to answer, and draining it
  // into a buffer nobody will read is just somewhere for an unbounded amount of
  // someone else's data to go. Stop reading, and let the pending replies go out.
  if (stop_reading_) return onSlotsChanged();

  char chunk[kReadChunk];
  for (;;) {
    const ssize_t n = ::recv(fd_.get(), chunk, sizeof(chunk), 0);
    if (n > 0) {
      read_.append(std::string_view(chunk, static_cast<std::size_t>(n)));
      if (!drainInput()) return false;
      // Level-triggered, so there is no obligation to read until EAGAIN: if
      // more is waiting, epoll says so again. One chunk per event keeps one
      // busy connection from starving the others.
      return onSlotsChanged();
    }
    if (n == 0) return false;  // peer closed
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return onSlotsChanged();
    return false;  // ECONNRESET and friends
  }
}

bool Connection::onWritable() { return flush(); }

bool Connection::wantsWrite() const { return !write_.empty(); }

std::size_t Connection::pendingWriteBytes() const { return write_.size(); }

bool Connection::drainInput() {
  for (;;) {
    if (stop_reading_) return true;

    // The views parse() fills point into read_. They stay valid only until
    // read_ is next mutated, so everything that must outlive this iteration is
    // copied during dispatch, and the bytes are consumed only afterwards.
    std::size_t consumed = 0;
    const ParseStatus status = parse(read_.readable(), argv_, consumed);

    if (status == ParseStatus::kNeedMore) return true;

    if (status == ParseStatus::kProtocolError) {
      // A terminal slot, not an immediate write. Earlier commands may still be
      // out at other shards, and the client is owed their replies first --
      // letting this jump the queue would break the one ordering guarantee RESP
      // gives.
      const std::uint32_t slot = slots_.reserve();
      std::string reply;
      resp::encodeError("ERR Protocol error: invalid request", reply);
      slots_.fill(slot, std::move(reply));
      close_after_flush_ = true;
      stop_reading_ = true;
      // The stream cannot be resynchronised: the framing is lost, so every
      // later byte would be guesswork.
      return true;
    }

    const std::uint32_t slot = slots_.reserve();
    const AfterCommand after =
        dispatch(*router_, slots_, slot, id_, argv_, *stats_);

    // Only now, once nothing borrows read_ any more.
    read_.consume(consumed);

    if (after == AfterCommand::kClose) {
      close_after_flush_ = true;
      stop_reading_ = true;
      return true;
    }
    if (read_.empty()) return true;
  }
}

// Moves whatever is now ready from the slots to the write buffer. The single
// place that decides what reaches the wire, so the ordering rule is one
// function rather than an obligation on every command path.
bool Connection::onSlotsChanged() {
  slots_.takeReadyPrefix(write_);
  return flush();
}

bool Connection::flush() {
  while (!write_.empty()) {
    const std::string_view pending = write_.readable();
    // MSG_NOSIGNAL, not write(): writing to a departed peer otherwise raises
    // SIGPIPE, whose default disposition kills the process. Local to this call
    // rather than a global signal(SIGPIPE, SIG_IGN).
    const ssize_t n = ::send(fd_.get(), pending.data(), pending.size(), MSG_NOSIGNAL);

    if (n > 0) {
      const bool short_write = static_cast<std::size_t>(n) < pending.size();
      write_.consume(static_cast<std::size_t>(n));
      if (short_write) ++stats_->short_writes;
      continue;  // the rest waits for the next EPOLLOUT
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Socket full. The loop registers EPOLLOUT because wantsWrite() is now
      // true, and drops it again once this drains.
      ++stats_->short_writes;
      updateBackpressure();
      return true;
    }
    if (errno == EPIPE || errno == ECONNRESET) ++stats_->peer_gone_writes;
    return false;  // the peer is gone; nothing left to do but close
  }

  updateBackpressure();

  // Close only once the terminal slot has actually gone out, and only once
  // nothing is still outstanding: a reply may yet arrive from another shard.
  return !(close_after_flush_ && slots_.idle());
}

// What backpressure measures is the residual after the send attempt, not the
// peak before it. A burst of replies the socket swallowed whole leaves nothing
// pending and is no reason to stop reading; pausing on the peak would throttle
// a connection that is keeping up perfectly.
void Connection::updateBackpressure() {
  if (!read_paused_ && write_.size() >= kWriteHighWatermark) {
    read_paused_ = true;
    ++stats_->read_pauses;
  } else if (read_paused_ && write_.size() <= kWriteLowWatermark) {
    read_paused_ = false;
  }
}

}  // namespace shardkv
