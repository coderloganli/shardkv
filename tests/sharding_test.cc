// Test cases 50-62 from task.md.
//
// A real multi-loop server: N threads, N shards, one port. Where routing_test
// pins the semantics exactly, this checks that the whole thing holds together
// once real sockets and real threads are involved.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "net/server.h"
#include "store/clock.h"
#include "store/hash.h"

using namespace shardkv;
using namespace std::chrono_literals;

namespace {

class Client {
 public:
  explicit Client(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd_, 0);
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(port);
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  }
  ~Client() { close(); }
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void send(std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
      const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, 0);
      ASSERT_GT(n, 0);
      sent += static_cast<std::size_t>(n);
    }
  }

  // Everything below reads through one buffer that persists across calls.
  //
  // Without it a read that needs a line can pull in bytes belonging to the next
  // reply and then throw them away, and the following read waits forever for
  // bytes it has already been handed. That is not hypothetical: it is what the
  // first version of readReply did.
  std::string receive(std::size_t n) {
    while (buffer_.size() < n && fill()) {
    }
    return take(std::min(n, buffer_.size()));
  }

  // Up to and including the first occurrence of the marker.
  std::string receiveUntilContains(std::string_view marker) {
    while (buffer_.find(marker) == std::string::npos) {
      if (!fill()) break;
    }
    const auto at = buffer_.find(marker);
    if (at == std::string::npos) return take(buffer_.size());
    return take(at + marker.size());
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  // Returns false once the peer stops sending.
  bool fill() {
    char buf[8192];
    const ssize_t got = ::recv(fd_, buf, sizeof(buf), 0);
    if (got <= 0) return false;
    buffer_.append(buf, static_cast<std::size_t>(got));
    return true;
  }

  std::string take(std::size_t n) {
    std::string out = buffer_.substr(0, n);
    buffer_.erase(0, n);
    return out;
  }

  int fd_ = -1;
  std::string buffer_;
};

// Starts a server on its own threads and stops it on destruction.
class Running {
 public:
  explicit Running(std::size_t shards, bool pin = false) {
    Server::Options options;
    options.port = 0;
    options.shards = shards;
    options.pin = pin;
    server_ = std::make_unique<Server>(options, clock_);
    server_->start();
  }
  ~Running() {
    if (server_) server_->stop();
  }

  std::uint16_t port() const { return server_->port(); }
  Server& server() { return *server_; }

 private:
  SteadyClock clock_;
  std::unique_ptr<Server> server_;
};

// Reads a complete RESP bulk string reply: the $<len> header, then exactly that
// many bytes plus the trailing CRLF.
//
// Reading until some marker appeared was a framing assumption dressed up as a
// read. INFO's body is one bulk string carrying every loop's fields, and
// stopping at the first of them only worked because loopback usually coalesces
// the rest into the same recv. The length is right there in the protocol.
std::string readBulkReply(Client& c) {
  std::string buffer = c.receiveUntilContains("\r\n");
  const auto header_end = buffer.find("\r\n");
  if (buffer.empty() || buffer[0] != '$' || header_end == std::string::npos) {
    ADD_FAILURE() << "not a bulk reply: " << buffer;
    return buffer;
  }

  const long long length = std::stoll(buffer.substr(1, header_end - 1));
  if (length < 0) return {};

  const std::size_t want = header_end + 2 + static_cast<std::size_t>(length) + 2;
  while (buffer.size() < want) {
    const std::string more = c.receive(want - buffer.size());
    if (more.empty()) break;
    buffer += more;
  }
  return buffer;
}

// Reads one complete RESP reply of any type.
//
// Needed because a script of mixed commands answers with a mix of shapes, and
// the alternative -- read whatever arrives within a timeout -- is not a read of
// the protocol at all. Under a sanitizer or a loaded machine the server can
// pause between replies, and a timeout then returns a prefix and calls it the
// whole answer.
std::string readReply(Client& c) {
  const std::string line = c.receiveUntilContains("\r\n");
  if (line.size() < 3) {
    ADD_FAILURE() << "no reply line: " << line;
    return line;
  }

  switch (line[0]) {
    case '+':
    case '-':
    case ':':
      return line;
    case '$': {
      const long long length = std::stoll(line.substr(1, line.size() - 3));
      if (length < 0) return line;  // $-1, a missing value
      return line + c.receive(static_cast<std::size_t>(length) + 2);
    }
    case '*': {
      // Element by element, because an array's total length is not in the
      // protocol -- only its element count is.
      const long long count = std::stoll(line.substr(1, line.size() - 3));
      std::string out = line;
      for (long long i = 0; i < count; ++i) out += readReply(c);
      return out;
    }
    default:
      ADD_FAILURE() << "unrecognised reply type: " << line;
      return line;
  }
}

std::string readReplies(Client& c, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) out += readReply(c);
  return out;
}

std::string infoOf(Client& c) {
  c.send("INFO\r\n");
  return readBulkReply(c);
}

long long fieldOf(const std::string& info, const std::string& name) {
  const auto at = info.find(name + ":");
  if (at == std::string::npos) return -1;
  const auto start = at + name.size() + 1;
  const auto eol = info.find("\r\n", start);
  if (eol == std::string::npos) return -1;
  return std::stoll(info.substr(start, eol - start));
}

// Which loop accepted this connection. The kernel decides that and the manual
// does not say how, so it is observed rather than assumed: exactly one
// connection is open, so exactly one loop reports a connection.
std::size_t loopOf(Client& c, std::size_t shards) {
  const std::string info = infoOf(c);
  for (std::size_t i = 0; i < shards; ++i) {
    if (fieldOf(info, "loop" + std::to_string(i) + "_connections") >= 1) return i;
  }
  ADD_FAILURE() << "no loop reported a connection:\n" << info;
  return 0;
}

std::string keyForShard(std::size_t want, std::size_t shards, std::string_view prefix = "k") {
  for (int i = 0; i < 100000; ++i) {
    std::string candidate = std::string(prefix) + std::to_string(i);
    if (shardForKey(candidate, shards) == want) return candidate;
  }
  ADD_FAILURE() << "no key found for shard " << want;
  return {};
}

std::string resp(const std::vector<std::string>& parts) {
  std::string out = "*" + std::to_string(parts.size()) + "\r\n";
  for (const auto& p : parts) {
    out += "$" + std::to_string(p.size()) + "\r\n" + p + "\r\n";
  }
  return out;
}

std::size_t openFdCount() {
  std::size_t n = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    (void)entry;
    ++n;
  }
  return n;
}

}  // namespace

// 50
TEST(Sharding, ServerWithFourShardsAnswersPing) {
  Running server(4);
  Client c(server.port());
  c.send("PING\r\n");
  EXPECT_EQ(c.receive(7), "+PONG\r\n");
}

// 51
TEST(Sharding, RemoteKeyRoundTripsOverTcp) {
  Running server(4);
  Client c(server.port());
  const std::size_t mine = loopOf(c, 4);
  const std::string remote = keyForShard((mine + 1) % 4, 4);

  c.send(resp({"SET", remote, "hello"}));
  EXPECT_EQ(c.receive(5), "+OK\r\n");
  c.send(resp({"GET", remote}));
  EXPECT_EQ(c.receive(11), "$5\r\nhello\r\n");
}

// 52 -- without this, 51 might have been a local hit all along and the whole
// group would be testing a path that never ran.
TEST(Sharding, CrossShardCounterIncreases) {
  Running server(4);
  Client c(server.port());
  const std::size_t mine = loopOf(c, 4);
  const std::string remote = keyForShard((mine + 1) % 4, 4);

  const long long before = fieldOf(infoOf(c), "cross_shard_requests");
  ASSERT_GE(before, 0);

  c.send(resp({"SET", remote, "v"}));
  ASSERT_EQ(c.receive(5), "+OK\r\n");

  EXPECT_GT(fieldOf(infoOf(c), "cross_shard_requests"), before);
}

// 53 -- the instrument is not free, so it is calibrated first.
//
// INFO is itself a cross-shard command: it asks every shard for its key count,
// so reading the counter with INFO adds (shards - 1) to the very thing being
// read. The first version of this test ignored that and failed, which is the
// test's fault and not the counter's -- "requests sent" should count every
// request sent, including INFO's own.
//
// So one INFO's cost is measured first, and then the local command is placed
// between two more. If a local key sent anything, the second interval would be
// larger than the first.
TEST(Sharding, LocalKeyDoesNotIncreaseCrossShardCounter) {
  Running server(4);
  Client c(server.port());
  const std::size_t mine = loopOf(c, 4);
  const std::string local = keyForShard(mine, 4);

  const long long a = fieldOf(infoOf(c), "cross_shard_requests");
  const long long b = fieldOf(infoOf(c), "cross_shard_requests");
  const long long info_cost = b - a;
  ASSERT_GT(info_cost, 0) << "INFO on four shards must fan out";

  c.send(resp({"SET", local, "v"}));
  ASSERT_EQ(c.receive(5), "+OK\r\n");

  const long long d = fieldOf(infoOf(c), "cross_shard_requests");
  EXPECT_EQ(d - b, info_cost) << "a local key sent a cross-shard request";
}

// 54
TEST(Sharding, PipelinedMixRepliesInOrderOverTcp) {
  Running server(4);
  Client c(server.port());
  const std::size_t mine = loopOf(c, 4);
  const std::string local = keyForShard(mine, 4, "L");
  const std::string remote = keyForShard((mine + 1) % 4, 4, "R");

  std::string batch;
  std::string expected;
  for (int i = 0; i < 20; ++i) {
    const std::string& key = (i % 2 == 0) ? local : remote;
    batch += resp({"SET", key, std::to_string(i)});
    expected += "+OK\r\n";
  }
  c.send(batch);
  EXPECT_EQ(c.receive(expected.size()), expected);
}

// 55
TEST(Sharding, ManyConnectionsAcrossLoopsAllCorrect) {
  Running server(4);
  const std::size_t before = openFdCount();
  {
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 100; ++i) clients.push_back(std::make_unique<Client>(server.port()));
    for (auto& c : clients) c->send("PING\r\n");
    for (auto& c : clients) EXPECT_EQ(c->receive(7), "+PONG\r\n");
  }
  std::this_thread::sleep_for(300ms);
  EXPECT_LE(openFdCount(), before + 2) << "descriptors leaked";
}

// 56 -- the answers must not depend on how many shards there are.
TEST(Sharding, FourShardsGiveByteIdenticalAnswersToOne) {
  const std::vector<std::vector<std::string>> script = {
      {"SET", "alpha", "1"},   {"SET", "beta", "2"},  {"GET", "alpha"},
      {"MGET", "alpha", "nope", "beta"}, {"INCR", "alpha"}, {"DEL", "beta", "nope"},
      {"EXISTS", "alpha", "alpha"}, {"DBSIZE"}, {"STRLEN", "alpha"},
  };

  auto runAll = [&](std::size_t shards) {
    Running server(shards);
    Client c(server.port());
    std::string batch;
    for (const auto& command : script) batch += resp(command);
    c.send(batch);
    // One complete reply per command, read by the protocol rather than by
    // waiting to see whether more turns up.
    return readReplies(c, script.size());
  };

  EXPECT_EQ(runAll(1), runAll(4));
}

// 57 -- covers the two-phase shutdown: stop while messages are in flight.
TEST(Sharding, ServerStopsCleanlyWithMessagesInFlight) {
  Running server(4);
  Client c(server.port());
  const std::size_t mine = loopOf(c, 4);
  const std::string remote = keyForShard((mine + 1) % 4, 4);

  std::string batch;
  for (int i = 0; i < 500; ++i) batch += resp({"SET", remote, std::to_string(i)});
  c.send(batch);
  // Stop without draining the replies: nodes are in flight right now.
  server.server().stop();
  SUCCEED() << "leaks are reported by ASan; a hang is reported by the test timing out";
}

// 58
TEST(Sharding, StopIsIdempotent) {
  Running server(2);
  server.server().stop();
  server.server().stop();
  SUCCEED();
}

// 59 -- the default, tested where the default lives.
//
// The first version of this set options.shards to the very expression it meant
// to check and then asserted they matched, which would have passed against any
// broken default at all. The rule now lives in defaultShardCount(), so the test
// can call the thing rather than restate it.
TEST(Options, ShardsDefaultsToOnePerCoreWithAFloorOfOne) {
  const std::size_t n = defaultShardCount();
  EXPECT_GE(n, 1u) << "hardware_concurrency() may return 0; 0 is not a shard count";

  const unsigned cores = std::thread::hardware_concurrency();
  if (cores > 0) EXPECT_EQ(n, static_cast<std::size_t>(cores));
}

// 60 -- argument parsing lives in main.cc; this pins the rule the parser must
// enforce rather than shelling out.
TEST(Options, ShardsRejectsZeroAndNegativeAndJunk) {
  EXPECT_FALSE(parseShardCountForTest("0").has_value());
  EXPECT_FALSE(parseShardCountForTest("-1").has_value());
  EXPECT_FALSE(parseShardCountForTest("4x").has_value());
  EXPECT_FALSE(parseShardCountForTest("").has_value());
  EXPECT_EQ(parseShardCountForTest("4").value_or(0), 4u);
}

// 61
TEST(Options, PinIsOffByDefault) {
  Running server(2);
  Client c(server.port());
  EXPECT_EQ(fieldOf(infoOf(c), "pinned"), 0);
}

// Added in stage 8. INFO reported pinned:1 while nothing was ever pinned -- a
// switch that is observable and does nothing is worse than no switch, because a
// measurement run would record an affinity it did not have.
//
// Twice strengthened after review. Counting every narrowed thread in the
// process cannot tell shardkv's loops from the test runner's own threads, so
// this measures the DELTA across starting the server: whatever narrowed while
// it came up is what it narrowed.
TEST(Options, PinNarrowsEveryLoopThreadOrReportsUnpinned) {
  constexpr std::size_t kShards = 2;

  auto narrowedThreads = [] {
    std::size_t n = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
      std::ifstream status(entry.path() / "status");
      std::string line;
      while (std::getline(status, line)) {
        if (line.rfind("Cpus_allowed_list:", 0) != 0) continue;
        const std::string list = line.substr(line.find(':') + 1);
        // One core is "3"; anything wider carries a dash or a comma.
        if (list.find('-') == std::string::npos && list.find(',') == std::string::npos) {
          ++n;
        }
      }
    }
    return n;
  };

  const std::size_t before = narrowedThreads();
  Running server(kShards, /*pin=*/true);
  Client c(server.port());
  const long long reported = fieldOf(infoOf(c), "pinned");

  // start() returns once the threads exist, not once they have run. Only the
  // loop that answered INFO is known to have reached its affinity call, so this
  // waits for the others rather than assuming they were scheduled -- a bounded
  // wait on something observable, not a sleep and a hope. If they never pin,
  // the deadline expires and the assertion below reports the shortfall.
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std::size_t gained = 0;
  for (;;) {
    const std::size_t after = narrowedThreads();
    gained = after > before ? after - before : 0;
    if (gained >= kShards) break;
    if (std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(10ms);
  }

  if (reported == 1) {
    // INFO is answered by whichever loop owns this connection and reports that
    // loop's own state, so a claim of pinned means at least that loop bound --
    // and since every loop runs the same code with the same request, all of
    // them should have.
    EXPECT_GE(gained, kShards)
        << "INFO says pinned, but starting the server narrowed only " << gained
        << " threads";
  } else {
    EXPECT_LT(gained, kShards)
        << "every loop bound to a core, but INFO does not say so";
  }
}

// 62
TEST(Sharding, SingleShardTakesNoCrossShardPath) {
  Running server(1);
  Client c(server.port());

  const std::vector<std::vector<std::string>> script = {
      {"SET", "a", "1"}, {"GET", "a"}, {"MGET", "a", "b"}, {"DEL", "a"}, {"DBSIZE"},
  };
  std::string batch;
  for (const auto& command : script) batch += resp(command);
  c.send(batch);
  (void)readReplies(c, script.size());

  EXPECT_EQ(fieldOf(infoOf(c), "cross_shard_requests"), 0)
      << "one shard means nothing is ever sent anywhere";
}
