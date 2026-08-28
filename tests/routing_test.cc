// Test cases 25-49 from task.md.
//
// Everything about cross-shard behaviour that can be pinned down exactly. No
// threads, no sockets: a TestRouter holds cross-shard requests and the test
// releases them when and in what order it likes, so "the remote reply has not
// come back yet" is a line of code rather than a sleep and a hope.
//
// See docs/adr/0008-routing-is-an-interface-so-ordering-can-be-tested.md

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "base/buffer.h"
#include "commands/dispatch.h"
#include "commands/router.h"
#include "helpers.h"
#include "net/connection.h"
#include "net/message.h"
#include "net/reply_slots.h"
#include "store/clock.h"
#include "store/hash.h"
#include "store/shard.h"

using namespace shardkv;
using shardkv::testing::kNoConnections;

namespace {

// Owns every shard, executes local work at once, and parks everything else
// until the test says otherwise.
class TestRouter final : public ShardRouter {
 public:
  TestRouter(std::size_t shards, std::size_t local, const Clock& clock)
      : local_(local) {
    for (std::size_t i = 0; i < shards; ++i) {
      shards_.push_back(std::make_unique<Shard>(clock));
    }
  }

  std::size_t shardCount() const override { return shards_.size(); }
  std::size_t localShard() const override { return local_; }
  Shard& local() override { return *shards_[local_]; }

  void send(std::size_t shard, CrossShardRequest request) override {
    held_.push_back({shard, std::move(request)});
  }

  std::size_t heldCount() const { return held_.size(); }
  const CrossShardRequest& held(std::size_t i) const { return held_[i].request; }

  // Runs one held request against its real shard and delivers the answer into
  // the slots, exactly as a loop would on the reply's return.
  void release(std::size_t index, ReplySlots& slots);
  void releaseAll(ReplySlots& slots);
  // Releases in the given order, so a test can choose the interleaving.
  void releaseInOrder(const std::vector<std::size_t>& order, ReplySlots& slots);

  Shard& shard(std::size_t i) { return *shards_[i]; }

 private:
  struct Held {
    std::size_t shard;
    CrossShardRequest request;
  };

  std::size_t local_;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::vector<Held> held_;
};

void TestRouter::release(std::size_t index, ReplySlots& slots) {
  // Production behaviour, called rather than reimplemented: a test that
  // reimplements the semantics tests its own copy of them.
  LoopFacts facts;
  facts.loops = shards_.size();
  executeCrossShardRequest(*shards_[held_[index].shard], held_[index].request, slots,
                           facts);
}

void TestRouter::releaseAll(ReplySlots& slots) {
  for (std::size_t i = 0; i < held_.size(); ++i) release(i, slots);
  held_.clear();
}

void TestRouter::releaseInOrder(const std::vector<std::size_t>& order, ReplySlots& slots) {
  for (const std::size_t i : order) release(i, slots);
  held_.clear();
}

// A connection's worth of state, without a connection.
struct Session {
  Session(std::size_t shards, std::size_t local)
      : router(shards, local, clock) {}

  SteadyClock clock;
  TestRouter router;
  ReplySlots slots;
  std::uint64_t conn_id = 7;

  AfterCommand send(const std::vector<std::string>& parts) {
    std::vector<std::string_view> argv;
    argv.reserve(parts.size());
    for (const auto& p : parts) argv.emplace_back(p);
    const std::uint32_t slot = slots.reserve();
    return dispatch(router, slots, slot, conn_id, argv, kNoConnections);
  }

  std::string flush() {
    Buffer out;
    slots.takeReadyPrefix(out);
    return std::string(out.readable());
  }
};

// A key that hashes to `want` with `shards` shards. Tests need to name a key
// that is definitely local or definitely remote, and guessing is not available.
std::string keyForShard(std::size_t want, std::size_t shards, std::string_view prefix = "k") {
  for (int i = 0; i < 100000; ++i) {
    std::string candidate = std::string(prefix) + std::to_string(i);
    if (shardForKey(candidate, shards) == want) return candidate;
  }
  ADD_FAILURE() << "no key found for shard " << want;
  return {};
}

}  // namespace

// ------------------------------------------------- 25-29 routing basics

// 25
TEST(Routing, LocalKeyTakesNoMessage) {
  Session s(4, 1);
  const std::string local = keyForShard(1, 4);
  s.send({"SET", local, "v"});
  EXPECT_EQ(s.router.heldCount(), 0u) << "a local key must not be sent anywhere";
  EXPECT_EQ(s.flush(), "+OK\r\n");
}

// 26
TEST(Routing, RemoteKeySendsExactlyOneMessage) {
  Session s(4, 1);
  const std::string remote = keyForShard(2, 4);
  s.send({"SET", remote, "v"});
  ASSERT_EQ(s.router.heldCount(), 1u);
  EXPECT_EQ(s.flush(), "") << "nothing may be written before the reply returns";

  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "+OK\r\n");
}

// 27 -- the message must own its bytes. The read buffer is reused long before
// the reply comes back, so a view here is the same mistake the parser's
// lifetime rule exists to prevent.
TEST(Routing, RemoteRequestCarriesOwnedBytes) {
  Session s(4, 1);
  const std::string remote = keyForShard(2, 4);
  const std::string value = "a-distinctive-value";

  std::vector<std::string> parts = {"SET", remote, value};
  std::vector<std::string_view> argv;
  for (const auto& p : parts) argv.emplace_back(p);

  const std::uint32_t slot = s.slots.reserve();
  dispatch(s.router, s.slots, slot, s.conn_id, argv, kNoConnections);

  ASSERT_EQ(s.router.heldCount(), 1u);
  const CrossShardRequest& request = s.router.held(0);
  for (std::size_t i = 0; i < request.argv.size(); ++i) {
    for (const auto& original : parts) {
      EXPECT_NE(request.argv[i].data(), original.data())
          << "argv[" << i << "] borrows the caller's storage";
    }
  }
}

// 28
TEST(Routing, AllSingleKeyCommandsWorkRemotely) {
  const std::string remote = keyForShard(2, 4);

  struct Case {
    std::vector<std::string> command;
    std::string expected;
  };
  const std::vector<Case> cases = {
      {{"SET", remote, "hello"}, "+OK\r\n"},
      {{"GET", remote}, "$5\r\nhello\r\n"},
      {{"APPEND", remote, "!"}, ":6\r\n"},
      {{"STRLEN", remote}, ":6\r\n"},
      {{"TYPE", remote}, "+string\r\n"},
      {{"GETSET", remote, "42"}, "$6\r\nhello!\r\n"},
      {{"INCR", remote}, ":43\r\n"},
      {{"DECR", remote}, ":42\r\n"},
      {{"EXPIRE", remote, "100"}, ":1\r\n"},
      {{"TTL", remote}, ":100\r\n"},
      {{"PERSIST", remote}, ":1\r\n"},
  };

  Session s(4, 1);
  for (const auto& c : cases) {
    s.send(c.command);
    s.router.releaseAll(s.slots);
    EXPECT_EQ(s.flush(), c.expected) << "command: " << c.command[0];
  }
}

// 29
TEST(Routing, ExpiryWorksOnARemoteShard) {
  Session s(4, 1);
  const std::string remote = keyForShard(3, 4);

  s.send({"SET", remote, "v", "EX", "100"});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "+OK\r\n");

  s.send({"TTL", remote});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), ":100\r\n");
}

// ------------------------------------------------------- 30-34 ordering

// 30
TEST(Ordering, RemoteThenLocalRepliesInOrder) {
  Session s(4, 1);
  const std::string remote = keyForShard(2, 4);
  const std::string local = keyForShard(1, 4);

  s.send({"SET", remote, "r"});
  s.send({"SET", local, "a"});
  s.send({"SET", local, "b"});
  s.send({"PING"});

  EXPECT_EQ(s.flush(), "") << "three ready replies are stuck behind one gap";

  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "+OK\r\n+OK\r\n+OK\r\n+PONG\r\n");
}

// 31
TEST(Ordering, LocalThenRemoteThenLocalRepliesInOrder) {
  Session s(4, 1);
  const std::string remote = keyForShard(2, 4);
  const std::string local = keyForShard(1, 4);

  s.send({"SET", local, "a"});
  s.send({"GET", remote});
  s.send({"PING"});

  EXPECT_EQ(s.flush(), "+OK\r\n") << "only the prefix before the gap";

  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "$-1\r\n+PONG\r\n");
}

// 32
TEST(Ordering, OutOfOrderReleaseStillRepliesInOrder) {
  Session s(4, 1);
  const std::string first = keyForShard(2, 4, "aa");
  const std::string second = keyForShard(3, 4, "bb");

  s.send({"SET", first, "1"});
  s.send({"SET", second, "2"});
  ASSERT_EQ(s.router.heldCount(), 2u);

  s.router.releaseInOrder({1, 0}, s.slots);
  EXPECT_EQ(s.flush(), "+OK\r\n+OK\r\n");
}

// 33
TEST(Ordering, ManyInterleavedCommandsReplyInOrder) {
  Session s(4, 1);
  const std::string local = keyForShard(1, 4);
  const std::string remote = keyForShard(2, 4);

  std::string expected;
  for (int i = 0; i < 20; ++i) {
    if (i % 2 == 0) {
      s.send({"SET", local, std::to_string(i)});
    } else {
      s.send({"SET", remote, std::to_string(i)});
    }
    expected += "+OK\r\n";
  }

  // Release the held requests back to front.
  std::vector<std::size_t> order;
  for (std::size_t i = s.router.heldCount(); i > 0; --i) order.push_back(i - 1);
  s.router.releaseInOrder(order, s.slots);

  EXPECT_EQ(s.flush(), expected);
}

// 34 -- proves the design did not quietly become "stop the connection on a
// cross-shard command".
TEST(Ordering, ParsingContinuesWhileASlotIsOutstanding) {
  Session s(4, 1);
  const std::string remote = keyForShard(2, 4);
  const std::string local = keyForShard(1, 4);

  s.send({"GET", remote});
  const std::size_t after_remote = s.slots.pendingForTest();

  s.send({"SET", local, "a"});
  s.send({"PING"});
  EXPECT_GT(s.slots.pendingForTest(), after_remote)
      << "later commands were not dispatched while a slot was outstanding";
}

// --------------------------------------------- 35-44 scatter and gather

// 35
TEST(ScatterGather, MgetSpanningShardsPreservesArgumentOrder) {
  Session s(4, 0);
  std::vector<std::string> keys;
  for (std::size_t shard = 0; shard < 4; ++shard) {
    keys.push_back(keyForShard(shard, 4, "x"));
    keys.push_back(keyForShard(shard, 4, "y"));
  }

  for (std::size_t i = 0; i < keys.size(); ++i) {
    s.send({"SET", keys[i], std::to_string(i)});
    s.router.releaseAll(s.slots);
    (void)s.flush();
  }

  std::vector<std::string> command = {"MGET"};
  for (const auto& k : keys) command.push_back(k);
  s.send(command);

  std::vector<std::size_t> order;
  for (std::size_t i = s.router.heldCount(); i > 0; --i) order.push_back(i - 1);
  s.router.releaseInOrder(order, s.slots);

  std::string expected = "*" + std::to_string(keys.size()) + "\r\n";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const std::string v = std::to_string(i);
    expected += "$" + std::to_string(v.size()) + "\r\n" + v + "\r\n";
  }
  EXPECT_EQ(s.flush(), expected);
}

// 36
TEST(ScatterGather, MgetWithMissingKeysAcrossShards) {
  Session s(4, 0);
  const std::string a = keyForShard(0, 4, "p");
  const std::string b = keyForShard(2, 4, "q");
  const std::string missing = keyForShard(3, 4, "r");

  s.send({"SET", a, "1"});
  s.router.releaseAll(s.slots);
  (void)s.flush();
  s.send({"SET", b, "2"});
  s.router.releaseAll(s.slots);
  (void)s.flush();

  s.send({"MGET", a, missing, b});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "*3\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n");
}

// 37
TEST(ScatterGather, MsetSpanningShardsSetsAll) {
  Session s(4, 0);
  const std::string a = keyForShard(0, 4, "m");
  const std::string b = keyForShard(1, 4, "n");
  const std::string c = keyForShard(3, 4, "o");

  s.send({"MSET", a, "1", b, "2", c, "3"});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "+OK\r\n");

  s.send({"MGET", a, b, c});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "*3\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n");
}

// 38 -- validation before grouping, so a bad call reaches no shard at all.
TEST(ScatterGather, MsetOddArgumentsRejectedBeforeAnyMessage) {
  Session s(4, 0);
  const std::string a = keyForShard(2, 4, "u");

  s.send({"MSET", a, "1", "dangling"});
  EXPECT_EQ(s.router.heldCount(), 0u) << "a rejected MSET must send nothing";
  EXPECT_EQ(s.flush(), "-ERR wrong number of arguments for 'mset' command\r\n");

  s.send({"GET", a});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "$-1\r\n") << "and must write nothing";
}

// 39
TEST(ScatterGather, DelSpanningShardsReturnsTotalCount) {
  Session s(4, 0);
  const std::string a = keyForShard(0, 4, "d");
  const std::string b = keyForShard(2, 4, "e");
  const std::string absent = keyForShard(3, 4, "f");

  s.send({"MSET", a, "1", b, "2"});
  s.router.releaseAll(s.slots);
  (void)s.flush();

  s.send({"DEL", a, b, absent});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), ":2\r\n");
}

// 40
TEST(ScatterGather, ExistsSpanningShardsCountsOccurrences) {
  Session s(4, 0);
  const std::string a = keyForShard(2, 4, "g");

  s.send({"SET", a, "1"});
  s.router.releaseAll(s.slots);
  (void)s.flush();

  s.send({"EXISTS", a, a});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), ":2\r\n") << "Redis counts occurrences, not distinct keys";
}

// 41
TEST(ScatterGather, MultiKeyAllOnOneShardTakesNoMessage) {
  Session s(4, 1);
  const std::string a = keyForShard(1, 4, "s");
  const std::string b = keyForShard(1, 4, "t");
  ASSERT_NE(a, b);

  s.send({"MSET", a, "1", b, "2"});
  EXPECT_EQ(s.router.heldCount(), 0u) << "all keys are local; nothing to send";
  EXPECT_EQ(s.flush(), "+OK\r\n");
}

// 42
TEST(ScatterGather, DbsizeSumsAllShards) {
  Session s(4, 0);
  for (std::size_t shard = 0; shard < 4; ++shard) {
    for (int i = 0; i < 5; ++i) {
      s.send({"SET", keyForShard(shard, 4, "z" + std::to_string(i)), "v"});
      s.router.releaseAll(s.slots);
      (void)s.flush();
    }
  }

  s.send({"DBSIZE"});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), ":20\r\n");
}

// 43
TEST(ScatterGather, FlushdbClearsEveryShard) {
  Session s(4, 0);
  const std::string a = keyForShard(0, 4, "h");
  const std::string b = keyForShard(3, 4, "i");

  s.send({"MSET", a, "1", b, "2"});
  s.router.releaseAll(s.slots);
  (void)s.flush();

  s.send({"FLUSHDB"});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "+OK\r\n");

  s.send({"DBSIZE"});
  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), ":0\r\n");
}

// 44
TEST(ScatterGather, InfoReportsEveryShardsKeyCount) {
  Session s(4, 0);
  s.send({"SET", keyForShard(0, 4, "j"), "v"});
  s.router.releaseAll(s.slots);
  (void)s.flush();
  s.send({"SET", keyForShard(2, 4, "j"), "v"});
  s.router.releaseAll(s.slots);
  (void)s.flush();

  s.send({"INFO"});
  s.router.releaseAll(s.slots);
  const std::string info = s.flush();

  for (int shard = 0; shard < 4; ++shard) {
    EXPECT_NE(info.find("shard" + std::to_string(shard) + "_keys:"), std::string::npos)
        << "shard " << shard << " missing from INFO:\n"
        << info;
  }
  EXPECT_NE(info.find("shard0_keys:1\r\n"), std::string::npos) << info;
  EXPECT_NE(info.find("shard2_keys:1\r\n"), std::string::npos) << info;
  EXPECT_NE(info.find("shard1_keys:0\r\n"), std::string::npos) << info;
}

// --------------------------------------------- 45-47 the terminal slot

// 45 -- QUIT must not overtake replies the client is still owed.
TEST(Terminal, QuitWaitsForOutstandingRemoteReplies) {
  Session s(4, 1);
  const std::string remote = keyForShard(2, 4);

  s.send({"GET", remote});
  const AfterCommand after = s.send({"QUIT"});

  EXPECT_EQ(after, AfterCommand::kClose);
  EXPECT_EQ(s.flush(), "") << "the +OK may not go out ahead of the GET's reply";

  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "$-1\r\n+OK\r\n");
}

// 46
TEST(Terminal, ProtocolErrorWaitsForOutstandingRemoteReplies) {
  Session s(4, 1);
  const std::string remote = keyForShard(2, 4);

  s.send({"GET", remote});

  // A protocol error takes a terminal slot in the same way a command does.
  const std::uint32_t slot = s.slots.reserve();
  s.slots.fill(slot, "-ERR Protocol error: invalid request\r\n");

  EXPECT_EQ(s.flush(), "") << "the error may not overtake the GET's reply";

  s.router.releaseAll(s.slots);
  EXPECT_EQ(s.flush(), "$-1\r\n-ERR Protocol error: invalid request\r\n");
}

// 47 -- the first version of this compared a value to itself and would have
// passed against a connection that kept right on reading. It now drives a real
// Connection over a socketpair, which is where the rule actually lives.
TEST(Terminal, NoCommandsAreParsedOrReadAfterATerminalSlot) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  SteadyClock clock;
  Shard shard(clock);
  shardkv::testing::LocalOnlyRouter router(shard);
  LoopStats stats;
  Connection connection(UniqueFd(fds[0]), router, stats, /*id=*/1);

  // QUIT, and then two perfectly good commands behind it.
  const std::string bytes = "QUIT\r\nPING\r\nPING\r\n";
  ASSERT_EQ(::send(fds[1], bytes.data(), bytes.size(), 0),
            static_cast<ssize_t>(bytes.size()));

  connection.onReadable();

  // Exactly one reply: the +OK. The PINGs are not ours to answer.
  char buf[256];
  const ssize_t got = ::recv(fds[1], buf, sizeof(buf), MSG_DONTWAIT);
  ASSERT_GT(got, 0);
  EXPECT_EQ(std::string(buf, static_cast<std::size_t>(got)), "+OK\r\n");

  // And nothing further is read from the socket, however much arrives.
  const std::string more(64 * 1024, 'x');
  (void)::send(fds[1], more.data(), more.size(), MSG_DONTWAIT);
  connection.onReadable();
  EXPECT_TRUE(connection.slots().idle())
      << "the connection kept parsing after a terminal slot";

  ::close(fds[1]);
}

// ----------------------------------------- 48-49 connection lifetime

// 48 -- a reply that arrives for a slot already gone must be dropped, not
// written somewhere. Driving it through ReplySlots directly, as the first
// version did, only showed that filling a stale slot is a no-op; the routing
// that decides which connection a reply belongs to is exercised end to end in
// sharding_test, where a client sends a remote command and disappears.
TEST(Lifetime, ReplyForADepartedSlotIsDropped) {
  ReplySlots slots;
  const std::uint32_t first = slots.reserve();
  slots.fill(first, "+OK\r\n");

  Buffer out;
  slots.takeReadyPrefix(out);
  ASSERT_EQ(std::string(out.readable()), "+OK\r\n");
  ASSERT_TRUE(slots.idle());

  // The slot number is now behind the base. A late reply naming it must not
  // land on whatever occupies that position next.
  const std::uint32_t second = slots.reserve();
  slots.fill(first, "-ERR a reply from the past\r\n");
  EXPECT_EQ(slots.pendingForTest(), 1u) << "the stale fill was applied";

  slots.fill(second, "+PONG\r\n");
  Buffer after;
  slots.takeReadyPrefix(after);
  EXPECT_EQ(std::string(after.readable()), "+PONG\r\n");
}

// 49
TEST(Lifetime, ConnectionIdsAreNeverReused) {
  // Ids come from a per-loop counter, so the pair (loop, id) is unique without
  // any shared atomic. What matters here is that the counter only ever rises.
  std::uint64_t previous = 0;
  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t id = nextConnectionIdForTest();
    EXPECT_GT(id, previous) << "connection ids must be strictly increasing";
    previous = id;
  }
}

// Added in stage 8. A command whose argument count cannot be right must not
// travel: the design says validation happens before any message is sent, and a
// round trip spent asking another loop a question already answerable here is
// both slower and a lie in the cross-shard counter.
TEST(Routing, WrongArityOnARemoteKeySendsNoMessage) {
  Session s(4, 1);
  const std::string remote = keyForShard(2, 4);

  s.send({"GET", remote, "extra"});
  EXPECT_EQ(s.router.heldCount(), 0u) << "a command that cannot be right travelled";
  EXPECT_EQ(s.flush(), "-ERR wrong number of arguments for 'get' command\r\n");

  s.send({"TTL", remote, "extra"});
  EXPECT_EQ(s.router.heldCount(), 0u);
  EXPECT_EQ(s.flush(), "-ERR wrong number of arguments for 'ttl' command\r\n");

  // And a count that IS acceptable still routes, so this did not simply refuse
  // everything.
  s.send({"GET", remote});
  EXPECT_EQ(s.router.heldCount(), 1u);
}

// Added in stage 8, after review. Stopping the reads was not enough: under
// level-triggered epoll a socket with unread bytes reports itself readable on
// every turn, so a connection that has a terminal slot but stays registered for
// EPOLLIN spins the loop until its last outstanding reply arrives.
//
// The connection must therefore stop *wanting* to read, not merely decline to.
TEST(Terminal, ATerminalSlotWithdrawsReadInterest) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  SteadyClock clock;
  Shard shard(clock);
  shardkv::testing::LocalOnlyRouter router(shard);
  LoopStats stats;
  Connection connection(UniqueFd(fds[0]), router, stats, /*id=*/1);

  EXPECT_TRUE(connection.wantsRead()) << "a fresh connection reads";

  const std::string quit = "QUIT\r\n";
  ASSERT_EQ(::send(fds[1], quit.data(), quit.size(), 0),
            static_cast<ssize_t>(quit.size()));
  connection.onReadable();

  EXPECT_FALSE(connection.wantsRead())
      << "read interest must be withdrawn, or the loop spins on unread bytes";

  ::close(fds[1]);
}

// Added in stage 8. INFO's shards: came from LoopStats rather than the router,
// which is two sources for one fact -- and they can disagree. With a
// four-shard router and default stats it reported one shard while fanning out
// over four.
TEST(ScatterGather, InfoReportsTheRoutersShardCount) {
  Session s(4, 2);
  s.send({"INFO"});
  s.router.releaseAll(s.slots);
  const std::string info = s.flush();
  EXPECT_NE(info.find("shards:4\r\n"), std::string::npos) << info;
}

// And every per-loop counter is labelled with the loop it came from, for all
// loops -- not the answering loop's value under a fixed loop0_ name.
TEST(ScatterGather, InfoLabelsEveryLoopsCounters) {
  Session s(4, 2);
  s.send({"INFO"});
  s.router.releaseAll(s.slots);
  const std::string info = s.flush();

  for (int loop = 0; loop < 4; ++loop) {
    const std::string n = std::to_string(loop);
    for (const char* field : {"_connections:", "_short_writes:",
                              "_peer_gone_writes:", "_cross_shard_requests:",
                              "_pinned:"}) {
      EXPECT_NE(info.find("loop" + n + field), std::string::npos)
          << "loop" << n << field << " missing from:\n"
          << info;
    }
  }
}
