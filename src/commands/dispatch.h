#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <optional>

#include "commands/router.h"
#include "net/message.h"
#include "net/reply_slots.h"
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

  // Requests sent to another loop. The only way, from outside, to tell a
  // cross-shard round trip from a lucky local hit -- without it, a test that
  // stores and reads back a "remote" key proves nothing about the path it took.
  std::size_t cross_shard_requests = 0;

  // Times a connection on this loop crossed the high watermark and stopped
  // being read from. Without it a test can show the server survived a slow
  // client without showing that backpressure was ever engaged, which proves
  // nothing.
  std::size_t read_pauses = 0;

  // Times accept() was refused a descriptor. Worth counting on its own terms,
  // and it is what makes "the listener is not being retried on every turn of
  // the loop" testable: the spin drives this up by millions in a fraction of a
  // second, the throttle by one a tick.
  std::size_t accept_failures = 0;

  // Reported by INFO so the pinning default is observable rather than assumed.
  bool pinned = false;
  std::size_t shard_count = 1;
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
// Runs one command, writing its answer into `slot`.
//
// A local command fills the slot before returning. A cross-shard one sends a
// message and returns with the slot still empty; it is filled later, by this
// same thread, when the reply comes back. Either way the connection flushes
// only the longest ready prefix, so replies leave in command order whatever
// order they finish in.
//
// argv holds views into the read buffer and must not outlive this call --
// anything a message carries is copied here. See proto/parser.h.
AfterCommand dispatch(ShardRouter& router,
                      ReplySlots& slots,
                      std::uint32_t slot,
                      std::uint64_t conn_id,
                      const std::vector<std::string_view>& argv,
                      const LoopStats& stats);

// Runs a request that arrived from another loop against the shard that owns the
// keys, and delivers the answer into the originating connection's slots.
//
// The owning loop calls the first half and the originating loop the second; the
// two are one function here because a test router drives both sides itself and
// reimplementing the semantics in the test would mean testing the copy rather
// than the code.
// The two numbers a loop knows that its shard does not. Passed in rather than
// reached for, so that running a request needs a shard and these, and nothing
// else -- which is what lets a test drive the same code with no loop at all.
struct LoopFacts {
  std::size_t connections = 0;
  std::size_t loops = 1;
  std::size_t short_writes = 0;
  std::size_t peer_gone_writes = 0;
  std::size_t cross_shard_requests = 0;
  std::size_t read_pauses = 0;
  std::size_t accept_failures = 0;
  bool pinned = false;
};

void executeCrossShardRequest(Shard& shard, const CrossShardRequest& request,
                              ReplySlots& slots, LoopFacts facts);

// Executes one command against one shard, with no routing and no slots. The
// whole of the single-threaded server's behaviour, reached both by a local
// command and by a request that arrived from another loop.
AfterCommand runOnShard(Shard& shard, const std::vector<std::string_view>& argv,
                        std::string& out, const LoopStats& stats);

// The decimal form of a number, as INFO and the counting commands need it.
std::string formatDecimal(std::int64_t value);

// Marks a counting group as DEL rather than EXISTS: both add up numbers, but
// one of them removes what it counts.
inline constexpr std::uint32_t kDeleteGroup = 1;

// Runs one request against the shard that owns its keys and hands the answer to
// `deliver`, which is either a slot on this thread or a message bound for
// another loop. A template so that both callers share the semantics rather than
// each keeping its own copy of them.
template <typename Deliver>
void runCrossShardRequest(Shard& shard, const CrossShardRequest& request,
                          LoopFacts facts, Deliver&& deliver) {
  switch (request.delivery) {
    case Delivery::kWhole: {
      std::vector<std::string_view> argv;
      argv.reserve(request.argv.size());
      for (const auto& part : request.argv) argv.emplace_back(part);
      std::string out;
      LoopStats unused;
      runOnShard(shard, argv, out, unused);
      deliver.whole(std::move(out));
      break;
    }
    case Delivery::kArrayParts: {
      for (std::size_t i = 0; i < request.indices.size(); ++i) {
        Value* value = shard.lookup(request.argv[i]);
        deliver.part(request.indices[i],
                     value != nullptr ? std::optional<std::string>(value->data)
                                      : std::nullopt);
      }
      break;
    }
    case Delivery::kCount: {
      std::int64_t count = 0;
      if (request.argv.empty()) {
        count = static_cast<std::int64_t>(shard.size());  // DBSIZE
      } else if (request.group == kDeleteGroup) {
        for (const auto& key : request.argv) {
          if (shard.lookup(key) != nullptr && shard.erase(key)) ++count;
        }
      } else {
        for (const auto& key : request.argv) {
          if (shard.lookup(key) != nullptr) ++count;
        }
      }
      deliver.count(count);
      break;
    }
    case Delivery::kLoopInfo: {
      // Every number INFO wants from this loop. All but the first belong to the
      // loop rather than the shard, which is why LoopFacts is a parameter
      // rather than something reached for here.
      //
      // Reporting these from whichever loop happened to answer, under a fixed
      // "loop0_" label, was a bug twice over: the value was one loop's and the
      // label was another's.
      const std::uint32_t loop = request.indices.empty() ? 0 : request.indices[0];
      const auto n = static_cast<std::uint32_t>(facts.loops);
      const auto at = [&](InfoField field) {
        return static_cast<std::uint32_t>(field) * n + loop;
      };
      deliver.part(at(InfoField::kKeys),
                   formatDecimal(static_cast<std::int64_t>(shard.size())));
      deliver.part(at(InfoField::kConnections),
                   formatDecimal(static_cast<std::int64_t>(facts.connections)));
      deliver.part(at(InfoField::kShortWrites),
                   formatDecimal(static_cast<std::int64_t>(facts.short_writes)));
      deliver.part(at(InfoField::kPeerGoneWrites),
                   formatDecimal(static_cast<std::int64_t>(facts.peer_gone_writes)));
      deliver.part(at(InfoField::kReadPauses),
                   formatDecimal(static_cast<std::int64_t>(facts.read_pauses)));
      deliver.part(at(InfoField::kAcceptFailures),
                   formatDecimal(static_cast<std::int64_t>(facts.accept_failures)));
      deliver.part(at(InfoField::kCrossShardRequests),
                   formatDecimal(static_cast<std::int64_t>(facts.cross_shard_requests)));
      deliver.part(at(InfoField::kPinned), facts.pinned ? "1" : "0");
      break;
    }
    case Delivery::kStatus: {
      if (request.argv.empty()) {
        shard.clear();  // FLUSHDB
      } else {
        for (std::size_t i = 0; i + 1 < request.argv.size(); i += 2) {
          shard.set(request.argv[i], request.argv[i + 1]);
        }
      }
      deliver.status();
      break;
    }
  }
}


// Connection identifiers come from a per-loop counter, never a shared one:
// messages already carry the originating loop, so the pair is unique without an
// atomic. Exposed for the test that pins the only property callers depend on --
// that it never goes backwards and never repeats.
std::uint64_t nextConnectionIdForTest();

}  // namespace shardkv
