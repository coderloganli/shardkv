#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "store/shard.h"

namespace shardkv {

// The counters INFO reports that do not come from the shard. The loop owns
// these, because the loop is the only thing that knows about connections -- a
// shard holds keys and has never heard of a socket.
struct LoopStats {
  std::size_t connections = 0;

  // Times a write to a client came up short and had to wait for the socket to
  // drain. Not instrumentation added for a test: it is the signal that a
  // client is not keeping up, which is exactly what the backpressure work will
  // act on. It happens to also be the only way to prove from outside that the
  // EPOLLOUT path ran, rather than hoping a large enough reply forced it.
  std::size_t short_writes = 0;

  // Writes abandoned because the peer had already gone -- EPIPE or
  // ECONNRESET. Worth counting on its own terms (a client that hangs up
  // mid-reply is a thing an operator wants to see), and it is the only direct
  // evidence that the write-to-a-departed-peer path was taken at all. Without
  // it, a test can show the process survived without showing it was ever put
  // at risk: that path is what MSG_NOSIGNAL protects, and unprotected it would
  // raise SIGPIPE and kill the process.
  std::size_t peer_gone_writes = 0;
};

// What the connection should do once a command has been answered.
enum class AfterCommand {
  kKeepOpen,
  // QUIT: the reply is written, then the connection closes.
  kClose,
};

// Runs one command against the shard, appending the RESP reply to `out`.
//
// argv holds views into the read buffer and must not be retained past this
// call -- anything that has to outlive it is copied into the shard here. See
// the lifetime rule in proto/parser.h.
//
// An unrecognised command is answered with -ERR unknown command 'X' and the
// connection stays open. That is not merely tidy: it is the protocol's
// documented downgrade path. A client opens with HELLO, and the way it learns
// the server speaks only RESP2 is by receiving that error and carrying on. A
// server that hangs on HELLO, or closes, fails the handshake rather than
// declining it.
AfterCommand dispatch(Shard& shard,
                      const std::vector<std::string_view>& argv,
                      std::string& out,
                      const LoopStats& stats);

}  // namespace shardkv
