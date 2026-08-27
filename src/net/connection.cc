#include "net/connection.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <utility>

#include "commands/dispatch.h"
#include "proto/encoder.h"
#include "proto/parser.h"

namespace shardkv {
namespace {
constexpr std::size_t kReadChunk = 16 * 1024;
}

Connection::Connection(UniqueFd fd, Shard& shard, LoopStats& stats)
    : fd_(std::move(fd)), shard_(&shard), stats_(&stats) {}

int Connection::fd() const { return fd_.get(); }

bool Connection::onReadable() {
  char chunk[kReadChunk];
  for (;;) {
    const ssize_t n = ::recv(fd_.get(), chunk, sizeof(chunk), 0);
    if (n > 0) {
      read_.append(std::string_view(chunk, static_cast<std::size_t>(n)));
      if (!drainInput()) return false;
      // Level-triggered, so there is no obligation to read until EAGAIN: if
      // more is waiting, epoll says so again. One chunk per event keeps one
      // busy connection from starving the others.
      return flush();
    }
    if (n == 0) return false;  // peer closed
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return flush();
    return false;  // ECONNRESET and friends
  }
}

bool Connection::onWritable() { return flush(); }

bool Connection::wantsWrite() const { return !write_.empty(); }

bool Connection::drainInput() {
  for (;;) {
    // The views parse() fills point into read_. They stay valid only until
    // read_ is next mutated, so everything that must outlive this iteration is
    // copied during dispatch, and the bytes are consumed only afterwards.
    std::size_t consumed = 0;
    const ParseStatus status = parse(read_.readable(), argv_, consumed);

    if (status == ParseStatus::kNeedMore) return true;

    if (status == ParseStatus::kProtocolError) {
      std::string reply;
      resp::encodeError("ERR Protocol error: invalid request", reply);
      write_.append(reply);
      close_after_flush_ = true;
      // The stream cannot be resynchronised: the framing is lost, so every
      // later byte would be guesswork.
      return true;
    }

    std::string reply;
    const AfterCommand after = dispatch(*shard_, argv_, reply, *stats_);
    write_.append(reply);

    // Only now, once nothing borrows read_ any more.
    read_.consume(consumed);

    if (after == AfterCommand::kClose) {
      close_after_flush_ = true;
      return true;
    }
    if (read_.empty()) return true;
  }
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
      return true;
    }
    if (errno == EPIPE || errno == ECONNRESET) ++stats_->peer_gone_writes;
    return false;  // the peer is gone; nothing left to do but close
  }

  return !close_after_flush_;
}

}  // namespace shardkv
