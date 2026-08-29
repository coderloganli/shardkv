// Test cases 95-102 from the single-threaded task, and 7-15, 27, 28-33 from the
// resource-management task.
//
// A real Loop on a real port, driven by real sockets. The loop binds port 0 so
// the kernel picks a free one and concurrent runs do not collide.
//
// The Backpressure cases drive a Connection over a socketpair with no loop
// behind it, the way ConnectionTest does: the pause state belongs to the
// connection, so driving it directly is what makes those cases deterministic.
// The FaultInjection cases lower RLIMIT_NOFILE, which is a property of the
// process rather than of a loop -- so each of them opens every descriptor it
// needs before lowering it, and restores the limit through a scope guard.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "helpers.h"
#include "net/connection.h"
#include "net/listener.h"
#include "net/loop.h"
#include "store/clock.h"
#include "store/shard.h"

using namespace shardkv;
using namespace std::chrono_literals;

namespace {

// Starts a Loop on its own thread and stops it on destruction.
class Server {
 public:
  Server() {
    table_.inboxes.push_back(std::make_unique<Inbox>());
    loop_ = std::make_unique<Loop>(0, shard_, 0, table_, /*pin=*/false);
    port_ = loop_->port();
    thread_ = std::thread([this] { loop_->run(); });
  }

  ~Server() {
    if (loop_) loop_->stop();
    if (thread_.joinable()) thread_.join();
  }

  std::uint16_t port() const { return port_; }

 private:
  SteadyClock clock_;
  Shard shard_{clock_};
  LoopTable table_;
  std::unique_ptr<Loop> loop_;
  std::thread thread_;
  std::uint16_t port_ = 0;
};

// A blocking client socket, which keeps the tests readable: the concurrency
// under test is the server's, not the client's.
class Client {
 public:
  explicit Client(std::uint16_t port, int recv_buffer_bytes = 0) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd_, 0);
    if (recv_buffer_bytes > 0) {
      // Set before connect so it is negotiated into the handshake.
      ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &recv_buffer_bytes,
                   sizeof(recv_buffer_bytes));
    }
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

  // Everything below reads through one buffer that persists across calls. A
  // read that needs a line can otherwise pull in bytes belonging to the next
  // reply and discard them, leaving the following read waiting for bytes it has
  // already been handed.
  std::string receive(std::size_t n) {
    while (buffer_.size() < n && fill()) {
    }
    return take(std::min(n, buffer_.size()));
  }

  std::string receiveUntilClosed() {
    while (fill()) {
    }
    return take(buffer_.size());
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

  bool peerClosed() {
    char c;
    return ::recv(fd_, &c, 1, 0) == 0;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int fd() const { return fd_; }

 private:
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

// Reads a complete RESP bulk string reply: the $<len> header, then exactly that
// many bytes plus the trailing CRLF.
//
// Reading until a marker appears is a framing assumption dressed up as a read:
// it works only when the recv that carries the marker happens to carry the
// value and its CRLF too. The length is in the protocol.
std::string readBulkReply(Client& c) {
  const std::string line = c.receiveUntilContains("\r\n");
  if (line.size() < 3 || line[0] != '$') {
    ADD_FAILURE() << "not a bulk reply: " << line;
    return line;
  }
  const long long length = std::stoll(line.substr(1, line.size() - 3));
  if (length < 0) return line;  // $-1, a missing value
  return line + c.receive(static_cast<std::size_t>(length) + 2);
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

// Reads one numeric field out of INFO, over a connection of its own. Returns
// -1 if the field is absent, so a caller's comparison fails loudly.
long long infoField(std::uint16_t port, const std::string& name) {
  Client probe(port);
  return fieldOf(infoOf(probe), name);
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

// 95
TEST(Integration, ServerAnswersPingOverTcp) {
  Server server;
  Client client(server.port());
  client.send("PING\r\n");
  EXPECT_EQ(client.receive(7), "+PONG\r\n");
}

// 96 -- several commands in one write, answered in order.
TEST(Integration, PipelinedCommandsAllAnswered) {
  Server server;
  Client client(server.port());
  client.send(
      "*1\r\n$4\r\nPING\r\n"
      "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
      "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
  const std::string expected = "+PONG\r\n+OK\r\n$1\r\nv\r\n";
  EXPECT_EQ(client.receive(expected.size()), expected);
}

// 97 -- one command split across two writes is still one command.
TEST(Integration, SplitCommandAcrossTwoWritesIsAnswered) {
  Server server;
  Client client(server.port());
  const std::string whole = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
  client.send(whole.substr(0, 12));
  std::this_thread::sleep_for(20ms);
  client.send(whole.substr(12));
  EXPECT_EQ(client.receive(5), "+OK\r\n");
}

// 98 -- the stream cannot be resynchronised after this, so the server says so
// and hangs up.
TEST(Integration, ProtocolErrorClosesConnection) {
  Server server;
  Client client(server.port());
  client.send("*-5\r\n");
  const std::string reply = client.receiveUntilClosed();
  EXPECT_EQ(reply.rfind("-ERR Protocol error", 0), 0u) << reply;
}

// 99 -- deterministic short write. A minimal receive buffer plus a client that
// does not read forces the server's write to come up short, so EPOLLOUT is
// exercised rather than hoped for. The loop must stay responsive meanwhile.
TEST(Integration, ShortWriteRegistersEpollout) {
  Server server;

  {
    Client setup(server.port());
    std::string set = "*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$1048576\r\n" +
                      std::string(1024 * 1024, 'x') + "\r\n";
    setup.send(set);
    EXPECT_EQ(setup.receive(5), "+OK\r\n");
  }

  Client slow(server.port(), /*recv_buffer_bytes=*/1024);
  slow.send("*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n");
  std::this_thread::sleep_for(100ms);  // let the server's write come up short

  // The loop must not be stuck on that connection.
  Client other(server.port());
  other.send("PING\r\n");
  EXPECT_EQ(other.receive(7), "+PONG\r\n");

  // And the short write must actually have happened. Shrinking the client's
  // receive buffer makes it very likely but not certain -- the server's own
  // send buffer is not under this test's control, and a reply that fitted in
  // it entirely would never come up short. An assertion about EPOLLOUT resting
  // on reply size alone would be a hope rather than a proof, so the counter is
  // the observable.
  EXPECT_GT(infoField(server.port(), "loop0_short_writes"), 0)
      << "the reply fitted in the socket buffer, so EPOLLOUT was never "
         "exercised and this test proved nothing";

  // And the slow reader eventually gets all of it, byte for byte. Checking only
  // the length would pass a resumed write that sent the right number of wrong
  // bytes, which is precisely the bug this path can have.
  const std::string expected_body(1024 * 1024, 'x');
  const std::string header = "$1048576\r\n";
  const std::string reply = slow.receive(header.size() + expected_body.size() + 2);
  ASSERT_EQ(reply.size(), header.size() + expected_body.size() + 2);
  EXPECT_EQ(reply.substr(0, header.size()), header);
  EXPECT_EQ(reply.substr(header.size(), expected_body.size()), expected_body);
  EXPECT_EQ(reply.substr(header.size() + expected_body.size()), "\r\n");
}

// 100 -- completeness only; case 99 is the one that pins EPOLLOUT.
TEST(Integration, LargeValueRoundTripsOverTcp) {
  Server server;
  Client client(server.port());
  const std::string value(1024 * 1024, 'q');
  client.send("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1048576\r\n" + value + "\r\n");
  EXPECT_EQ(client.receive(5), "+OK\r\n");

  client.send("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
  const std::string header = "$1048576\r\n";
  const std::string reply = client.receive(header.size() + value.size() + 2);
  ASSERT_EQ(reply.size(), header.size() + value.size() + 2);
  EXPECT_EQ(reply.substr(header.size(), value.size()), value);
}

// 101 -- and no descriptor left behind.
TEST(Integration, ManyConcurrentConnections) {
  Server server;
  const std::size_t before = openFdCount();

  {
    std::vector<std::unique_ptr<Client>> clients;
    for (int i = 0; i < 100; ++i) {
      clients.push_back(std::make_unique<Client>(server.port()));
    }
    // Several commands each, not one: what this is for is repeated parsing and
    // buffer reuse across a connection's life, under concurrency. One PING per
    // client would exercise none of that.
    for (int round = 0; round < 5; ++round) {
      for (std::size_t c = 0; c < clients.size(); ++c) {
        const std::string key = "k" + std::to_string(c);
        const std::string value = "v" + std::to_string(round);
        clients[c]->send("*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" +
                         key + "\r\n$" + std::to_string(value.size()) + "\r\n" +
                         value + "\r\n");
        clients[c]->send("*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" +
                         key + "\r\n");
        clients[c]->send("PING\r\n");
      }
      for (std::size_t c = 0; c < clients.size(); ++c) {
        const std::string value = "v" + std::to_string(round);
        const std::string expected = "+OK\r\n$" + std::to_string(value.size()) +
                                     "\r\n" + value + "\r\n+PONG\r\n";
        EXPECT_EQ(clients[c]->receive(expected.size()), expected)
            << "client " << c << ", round " << round;
      }
    }
  }

  std::this_thread::sleep_for(200ms);  // let the loop reap them
  EXPECT_LE(openFdCount(), before + 2) << "descriptors leaked";
}

// 102 -- writing to a departed peer raises SIGPIPE, whose default disposition
// kills the process. MSG_NOSIGNAL is what stops that, and this is the test of
// it. If this test kills the runner, that is the bug.
//
// What this test can honestly claim is bounded, and the boundary is worth
// stating rather than papering over.
//
// Two earlier versions of it asserted more than they proved. Closing right
// after sending races: the server may finish the whole reply before the close
// lands. Asserting loop0_short_writes first is no better -- it shows a short
// write happened at some earlier moment, not that bytes were still pending
// when the peer went, since EPOLLOUT may have drained them while the probe was
// in flight.
//
// Asserting loop0_peer_gone_writes here was tried too, and it failed
// intermittently, for a structural reason rather than a timing one: a peer
// that closes with unread data sends RST, epoll reports EPOLLERR, and the loop
// tears the connection down without attempting the pending write. Through a
// real socket that path is a narrow race, not something a client can force.
//
// So this test asserts what it can: a client vanishing mid-reply does not take
// the server with it, and the connection is reaped. The send() that would
// raise SIGPIPE is pinned exactly, on a socketpair, in
// ConnectionTest.WriteToDepartedPeerIsAnErrorNotASignal below.
TEST(Integration, ClosedPeerDoesNotKillServer) {
  Server server;

  {
    Client setup(server.port());
    const std::string value(1024 * 1024, 'z');
    setup.send("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1048576\r\n" + value + "\r\n");
    ASSERT_EQ(setup.receive(5), "+OK\r\n");
  }

  {
    // A minimal receive buffer and a client that never reads, so the server is
    // certainly mid-reply when this one hangs up.
    Client rude(server.port(), /*recv_buffer_bytes=*/1024);
    rude.send("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
    std::this_thread::sleep_for(100ms);
    ASSERT_GT(infoField(server.port(), "loop0_short_writes"), 0)
        << "the reply was never pending, so the client did not leave mid-reply";
  }

  std::this_thread::sleep_for(200ms);  // let the loop reap it

  // Still here, still serving, and the dead connection is gone.
  Client other(server.port());
  other.send("PING\r\n");
  EXPECT_EQ(other.receive(7), "+PONG\r\n");
  EXPECT_EQ(infoField(server.port(), "loop0_connections"), 2)
      << "the departed client's connection was not reaped";
}

// Added in stage 8. Case 90 asserts that QUIT is followed by the server
// closing the connection, but the dispatch-level test can only see the
// AfterCommand value -- a loop that ignored it would still pass. This is the
// half of case 90 that needs a socket.
TEST(Integration, QuitClosesTheConnection) {
  Server server;
  Client client(server.port());
  client.send("QUIT\r\n");
  EXPECT_EQ(client.receive(5), "+OK\r\n");
  EXPECT_TRUE(client.peerClosed()) << "the server must close after replying";
}

// Added in stage 8. Case 68 checks that INFO's connection field parses as an
// integer, which a hardcoded 0 satisfied -- and did, until this was written.
// The live count can only be observed over TCP.
TEST(Integration, InfoReportsLiveConnectionCount) {
  Server server;
  Client first(server.port());

  EXPECT_EQ(fieldOf(infoOf(first), "loop0_connections"), 1);

  {
    Client second(server.port());
    second.send("PING\r\n");
    ASSERT_EQ(second.receive(7), "+PONG\r\n");  // ensure it is accepted first
    EXPECT_EQ(fieldOf(infoOf(first), "loop0_connections"), 2);
  }

  std::this_thread::sleep_for(200ms);  // let the loop reap the closed one
  EXPECT_EQ(fieldOf(infoOf(first), "loop0_connections"), 1);
}

// The SIGPIPE protection, tested where it can actually be pinned down.
//
// Case 102 above is about the process surviving; this is about the specific
// send() that would kill it. Driving that through a real loop turned out to be
// unreliable, and for a structural reason worth writing down: when a peer
// closes with unread data it sends RST, epoll reports EPOLLERR, and the loop
// tears the connection down without attempting the pending write. So the
// write-to-a-departed-peer path is a narrow race there -- real, and fatal
// without MSG_NOSIGNAL, but not something an external client can force.
//
// A socketpair makes it exact. The command is queued before the peer closes,
// so the connection is certain to read it, answer it, and write into a socket
// whose peer has gone. Without MSG_NOSIGNAL this test does not fail -- it
// takes the whole test binary down with SIGPIPE, which is the point.
TEST(ConnectionTest, WriteToDepartedPeerIsAnErrorNotASignal) {
  int fds[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

  const std::string command = "PING\r\n";
  ASSERT_EQ(::send(fds[1], command.data(), command.size(), 0),
            static_cast<ssize_t>(command.size()));
  ASSERT_EQ(::close(fds[1]), 0);  // peer gone; the command is already queued

  SteadyClock clock;
  Shard shard(clock);
  LoopStats stats;
  shardkv::testing::LocalOnlyRouter router(shard);
  Connection connection(UniqueFd(fds[0]), router, stats, /*id=*/1);

  // Reads the queued command, answers it, and writes to the dead peer.
  EXPECT_FALSE(connection.onReadable()) << "the connection should report itself finished";
  EXPECT_EQ(stats.peer_gone_writes, 1u)
      << "the write to the departed peer was never attempted";
}

// ---------------------------------------------------------------------------
// Cases 7-15, 27 and 28-33 from the resource-management task.
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kBigValue = 1024 * 1024;
constexpr std::size_t kHighWatermark = 1024 * 1024;
constexpr std::size_t kLowWatermark = 256 * 1024;

void setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ASSERT_GE(flags, 0);
  ASSERT_EQ(::fcntl(fd, F_SETFL, flags | O_NONBLOCK), 0);
}

// A Connection over a socketpair, with a peer that reads only when told to.
//
// No loop behind it, the way ConnectionTest already works: the pause state is a
// property of the connection, and driving it directly is what makes these cases
// deterministic. Both ends are non-blocking, because a blocking send() of eight
// megabytes would hang the test instead of coming up short.
class SlowPeer {
 public:
  SlowPeer() {
    int fds[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    peer_ = fds[1];
    setNonBlocking(fds[0]);
    setNonBlocking(peer_);

    // A modest send buffer, so that draining happens in many small steps and
    // the hysteresis case gets observations between the two watermarks.
    const int sndbuf = 64 * 1024;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    shard_.set("big", std::string(kBigValue, 'x'));
    connection_ = std::make_unique<Connection>(UniqueFd(fds[0]), router_, stats_,
                                               /*id=*/1);
  }

  ~SlowPeer() {
    if (peer_ >= 0) ::close(peer_);
  }

  // Pipelines n copies of `GET big`, then lets the connection read them. One
  // reply is a megabyte, so eight of them are an order of magnitude more than
  // any socketpair will absorb -- whatever the kernel accepts, the residual is
  // far above the high watermark.
  bool pipelineGets(int n) {
    std::string commands;
    for (int i = 0; i < n; ++i) commands += "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n";
    const ssize_t sent = ::send(peer_, commands.data(), commands.size(), 0);
    EXPECT_EQ(sent, static_cast<ssize_t>(commands.size()));
    return connection_->onReadable();
  }

  bool sendLine(std::string_view line) {
    EXPECT_EQ(::send(peer_, line.data(), line.size(), 0),
              static_cast<ssize_t>(line.size()));
    return connection_->onReadable();
  }

  // Reads whatever the peer's socket currently holds, appending it to what the
  // peer has seen so far. Returns how many bytes came out.
  std::size_t readAvailable() {
    std::size_t total = 0;
    char buf[64 * 1024];
    for (;;) {
      const ssize_t got = ::recv(peer_, buf, sizeof(buf), 0);
      if (got <= 0) break;
      received_.append(buf, static_cast<std::size_t>(got));
      total += static_cast<std::size_t>(got);
    }
    return total;
  }

  // One drain step: take what the peer can, then let the connection write more.
  bool drainStep() {
    readAvailable();
    return connection_->onWritable();
  }

  // Drains to empty, or gives up. Returns false if it gave up, so a caller
  // asserts rather than looping forever on a bug.
  bool drainFully(int max_steps = 100000) {
    for (int i = 0; i < max_steps; ++i) {
      if (connection_->pendingWriteBytes() == 0) return true;
      if (!drainStep()) return false;
    }
    return false;
  }

  Connection& connection() { return *connection_; }
  const LoopStats& stats() const { return stats_; }
  const std::string& received() const { return received_; }

  static std::string expectedBigReply() {
    return "$" + std::to_string(kBigValue) + "\r\n" + std::string(kBigValue, 'x') +
           "\r\n";
  }

 private:
  SteadyClock clock_;
  Shard shard_{clock_};
  shardkv::testing::LocalOnlyRouter router_{shard_};
  LoopStats stats_;
  std::unique_ptr<Connection> connection_;
  int peer_ = -1;
  std::string received_;
};

}  // namespace

// 7
TEST(Backpressure, BelowTheHighWatermarkTheConnectionKeepsReading) {
  SlowPeer peer;
  EXPECT_TRUE(peer.sendLine("PING\r\n"));

  EXPECT_LT(peer.connection().pendingWriteBytes(), kHighWatermark);
  EXPECT_FALSE(peer.connection().readPaused());
  EXPECT_TRUE(peer.connection().wantsRead());
}

// 8 -- the feature. The assertion is about the residual the test observed, not
// about a byte count the test arranged, because how much send() accepts is the
// kernel's business.
TEST(Backpressure, CrossingTheHighWatermarkStopsReading) {
  SlowPeer peer;
  EXPECT_TRUE(peer.pipelineGets(8));

  ASSERT_GE(peer.connection().pendingWriteBytes(), kHighWatermark)
      << "the socket absorbed more than eight megabytes, so this case never "
         "reached the watermark and proved nothing";
  EXPECT_TRUE(peer.connection().readPaused());
  EXPECT_FALSE(peer.connection().wantsRead());
  EXPECT_EQ(peer.stats().read_pauses, 1u);
}

// 9 -- the invariant that keeps backpressure from hanging a client forever.
// Unlike a connection stopped by QUIT, a paused one has no second mover: only
// its own socket draining can resume it, and only EPOLLOUT reports that.
TEST(Backpressure, APausedConnectionAlwaysStillWantsToWrite) {
  SlowPeer peer;
  EXPECT_TRUE(peer.pipelineGets(8));

  ASSERT_TRUE(peer.connection().readPaused());
  EXPECT_TRUE(peer.connection().wantsWrite())
      << "paused with nothing to write: the loop would register interest in "
         "nothing and never wake this connection again";
}

// 10
TEST(Backpressure, DrainingFullyResumesReading) {
  SlowPeer peer;
  EXPECT_TRUE(peer.pipelineGets(8));
  ASSERT_TRUE(peer.connection().readPaused());

  ASSERT_TRUE(peer.drainFully());
  EXPECT_EQ(peer.connection().pendingWriteBytes(), 0u);
  EXPECT_FALSE(peer.connection().readPaused());
  EXPECT_TRUE(peer.connection().wantsRead());
}

// 11 -- hysteresis. Reading must not resume until the buffer is at or below the
// low watermark, so every observation above it must still be paused.
TEST(Backpressure, ReadingDoesNotResumeAboveTheLowWatermark) {
  SlowPeer peer;
  EXPECT_TRUE(peer.pipelineGets(8));
  ASSERT_TRUE(peer.connection().readPaused());

  int observations_in_the_band = 0;
  for (int i = 0; i < 100000; ++i) {
    const std::size_t pending = peer.connection().pendingWriteBytes();
    if (pending == 0) break;
    if (pending > kLowWatermark) {
      ASSERT_TRUE(peer.connection().readPaused())
          << "resumed with " << pending << " bytes still pending, which is "
          << "above the low watermark";
      if (pending < kHighWatermark) ++observations_in_the_band;
    }
    ASSERT_TRUE(peer.drainStep());
  }

  EXPECT_GT(observations_in_the_band, 0)
      << "no observation landed between the two watermarks, so this case never "
         "tested hysteresis at all";
}

// 12 -- pairs with case 11: one proves it does not resume early, this one that
// it can pause again at all.
TEST(Backpressure, ASecondCrossingPausesAgainAndCountsAgain) {
  SlowPeer peer;
  EXPECT_TRUE(peer.pipelineGets(8));
  ASSERT_EQ(peer.stats().read_pauses, 1u);

  ASSERT_TRUE(peer.drainFully());
  ASSERT_FALSE(peer.connection().readPaused());

  EXPECT_TRUE(peer.pipelineGets(8));
  EXPECT_TRUE(peer.connection().readPaused());
  EXPECT_EQ(peer.stats().read_pauses, 2u);
}

// 13 -- moving bytes around under a stalled writer is how this loses data.
TEST(Backpressure, PausingLosesAndReordersNothing) {
  SlowPeer peer;
  const int kCommands = 8;
  EXPECT_TRUE(peer.pipelineGets(kCommands));
  ASSERT_TRUE(peer.connection().readPaused());

  ASSERT_TRUE(peer.drainFully());
  peer.readAvailable();

  std::string expected;
  for (int i = 0; i < kCommands; ++i) expected += SlowPeer::expectedBigReply();
  EXPECT_EQ(peer.received().size(), expected.size());
  EXPECT_EQ(peer.received(), expected);
}

// 14 -- the same thing over TCP, against the real loop. The Server fixture
// builds one loop, which is what makes loop0_read_pauses unambiguous.
TEST(Integration, BackpressureIsVisibleOverTheWire) {
  Server server;

  {
    Client setup(server.port());
    setup.send("*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$1048576\r\n" +
               std::string(kBigValue, 'x') + "\r\n");
    EXPECT_EQ(setup.receive(5), "+OK\r\n");
  }

  Client slow(server.port(), /*recv_buffer_bytes=*/1024);
  std::string commands;
  for (int i = 0; i < 8; ++i) commands += "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n";
  slow.send(commands);
  std::this_thread::sleep_for(200ms);  // let the server fill its write buffer

  EXPECT_GT(infoField(server.port(), "loop0_read_pauses"), 0)
      << "eight megabytes of replies to a client that never reads did not "
         "engage backpressure";

  Client other(server.port());
  other.send("PING\r\n");
  EXPECT_EQ(other.receive(7), "+PONG\r\n") << "the loop is stuck on the slow client";
}

// 15
TEST(Integration, APausedClientRecovers) {
  Server server;

  {
    Client setup(server.port());
    setup.send("*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$1048576\r\n" +
               std::string(kBigValue, 'x') + "\r\n");
    EXPECT_EQ(setup.receive(5), "+OK\r\n");
  }

  Client slow(server.port(), /*recv_buffer_bytes=*/1024);
  const int kCommands = 8;
  std::string commands;
  for (int i = 0; i < kCommands; ++i) commands += "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n";
  slow.send(commands);
  std::this_thread::sleep_for(200ms);
  ASSERT_GT(infoField(server.port(), "loop0_read_pauses"), 0);

  const std::size_t one = std::string("$1048576\r\n").size() + kBigValue + 2;
  const std::string all = slow.receive(one * kCommands);
  ASSERT_EQ(all.size(), one * kCommands);

  slow.send("PING\r\n");
  EXPECT_EQ(slow.receive(7), "+PONG\r\n")
      << "the connection never resumed being read from";
}

// 27 -- the only case that proves the timer is wired to the sampling pass.
// Every other expiry case calls the pass by hand. One loop means one shard, so
// no hashing assumption is involved in reading shard0_keys.
TEST(Integration, TheTimerDrivesSampledExpiry) {
  Server server;

  {
    Client setup(server.port());
    setup.send("*5\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n$2\r\nEX\r\n$1\r\n1\r\n");
    EXPECT_EQ(setup.receive(5), "+OK\r\n");
    // And it is really there, so a later zero means reaped rather than never
    // stored.
    EXPECT_EQ(fieldOf(infoOf(setup), "shard0_keys"), 1);
  }

  // k is never accessed again, so only the sampler can free it.
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  long long keys = -1;
  while (std::chrono::steady_clock::now() < deadline) {
    keys = infoField(server.port(), "shard0_keys");
    if (keys == 0) break;
    std::this_thread::sleep_for(50ms);
  }

  EXPECT_EQ(keys, 0)
      << "five seconds after a one-second TTL, an untouched expired key is "
         "still counted: the sampler is not being called";
}

// 28 -- a client that vanishes while the server is mid-reply.
TEST(Integration, AClientKilledMidWriteLeaksNothing) {
  Server server;

  {
    Client setup(server.port());
    setup.send("*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$1048576\r\n" +
               std::string(kBigValue, 'x') + "\r\n");
    EXPECT_EQ(setup.receive(5), "+OK\r\n");
  }

  const std::size_t before = openFdCount();

  {
    Client victim(server.port(), /*recv_buffer_bytes=*/1024);
    victim.send("*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n");
    std::this_thread::sleep_for(100ms);  // the server is now mid-write

    // SO_LINGER with a zero timeout makes close() send an RST rather than a
    // FIN, so the server's next write meets ECONNRESET instead of a tidy
    // shutdown. That is the path MSG_NOSIGNAL protects.
    linger no_linger{1, 0};
    ::setsockopt(victim.fd(), SOL_SOCKET, SO_LINGER, &no_linger, sizeof(no_linger));
  }

  Client other(server.port());
  other.send("PING\r\n");
  EXPECT_EQ(other.receive(7), "+PONG\r\n") << "the server did not survive the kill";

  // The evidence that the client really was killed MID-write, rather than after
  // the reply had all gone out: the server came up short on that connection, so
  // it still held pending bytes when the peer vanished.
  //
  // What is deliberately not asserted here is loop0_peer_gone_writes. Whether
  // the server meets the RST as a failed send() or as EPOLLHUP on the next turn
  // of the loop is a race between the reset arriving and the loop waking, and
  // both outcomes are correct. The write-to-a-departed-peer path is pinned
  // deterministically by ConnectionTest.WriteToDepartedPeerIsAnErrorNotASignal
  // instead; this case is about surviving and not leaking.
  EXPECT_GT(infoField(server.port(), "loop0_short_writes"), 0)
      << "the reply went out whole, so the client was not killed mid-write and "
         "this case did not exercise what it is named for";
  other.close();

  // The loop closes the connection on its own thread, so give it a moment.
  std::size_t after = openFdCount();
  for (int i = 0; i < 50 && after > before; ++i) {
    std::this_thread::sleep_for(20ms);
    after = openFdCount();
  }
  EXPECT_LE(after, before) << "a descriptor was leaked by the killed connection";
}

// 29 -- one leak can hide in the slack of a single measurement.
TEST(Integration, AHundredKillsLeaveTheDescriptorCountFlat) {
  Server server;

  {
    Client setup(server.port());
    setup.send("*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$1048576\r\n" +
               std::string(kBigValue, 'x') + "\r\n");
    EXPECT_EQ(setup.receive(5), "+OK\r\n");
  }

  const std::size_t before = openFdCount();

  for (int i = 0; i < 100; ++i) {
    Client victim(server.port(), /*recv_buffer_bytes=*/1024);
    victim.send("*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n");
    linger no_linger{1, 0};
    ::setsockopt(victim.fd(), SOL_SOCKET, SO_LINGER, &no_linger, sizeof(no_linger));
  }

  Client other(server.port());
  other.send("PING\r\n");
  EXPECT_EQ(other.receive(7), "+PONG\r\n");
  other.close();

  std::size_t after = openFdCount();
  for (int i = 0; i < 100 && after > before; ++i) {
    std::this_thread::sleep_for(20ms);
    after = openFdCount();
  }
  EXPECT_LE(after, before) << "descriptors accumulated over a hundred kills";
}

namespace {

// Restores RLIMIT_NOFILE however the test leaves. A failed assertion must not
// leave the rest of the binary unable to open a file.
class DescriptorLimit {
 public:
  DescriptorLimit() { ::getrlimit(RLIMIT_NOFILE, &saved_); }
  ~DescriptorLimit() { restore(); }

  DescriptorLimit(const DescriptorLimit&) = delete;
  DescriptorLimit& operator=(const DescriptorLimit&) = delete;

  // Leaves the process genuinely unable to allocate a descriptor.
  //
  // Lowering the limit alone is not enough, and finding that out is what this
  // comment is for. RLIMIT_NOFILE bounds descriptor NUMBERS, not how many are
  // in use, and the numbers a process holds are not contiguous -- so capping at
  // one past the highest still leaves every gap below it free, and accept()
  // succeeds. So: cap the ceiling, then take every remaining slot with dup()
  // until that fails too.
  //
  // The scan of /proc happens before the limit drops, because reading a
  // directory needs a descriptor of its own.
  bool exhaust() {
    int highest = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      highest = std::max(highest, std::atoi(entry.path().filename().c_str()));
    }
    rlimit lowered = saved_;
    lowered.rlim_cur = static_cast<rlim_t>(highest + 1);
    if (::setrlimit(RLIMIT_NOFILE, &lowered) != 0) return false;

    for (;;) {
      const int hog = ::dup(0);
      if (hog < 0) break;
      hogs_.push_back(hog);
    }
    // One more must fail, or the process is not actually out of descriptors and
    // every assertion after this would be about nothing.
    const int check = ::dup(0);
    if (check >= 0) {
      ::close(check);
      return false;
    }
    return true;
  }

  void restore() {
    for (const int hog : hogs_) ::close(hog);
    hogs_.clear();
    ::setrlimit(RLIMIT_NOFILE, &saved_);
  }

 private:
  rlimit saved_{};
  std::vector<int> hogs_;
};

// Connects an already-created socket, which is what lets these cases put a
// connection in the backlog while no descriptor can be allocated.
bool connectTo(int fd, std::uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = ::htons(port);
  addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  return ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
}

}  // namespace

// 30 -- EMFILE is not EAGAIN. One means the backlog is empty and the loop may
// sleep; the other means it is not empty and there was no descriptor to accept
// into. Both halves are asserted, because a flag that were set unconditionally
// would pass on the first alone.
TEST(FaultInjection, AcceptReportsDescriptorExhaustion) {
  Listener listener(0);

  bool out_of_descriptors = true;
  UniqueFd nothing = listener.accept(&out_of_descriptors);
  EXPECT_FALSE(nothing) << "nothing was pending";
  EXPECT_FALSE(out_of_descriptors)
      << "an empty backlog was reported as descriptor exhaustion";

  // Everything this case needs is opened before the limit drops. UniqueFd
  // rather than a raw int, so an assertion that returns early cannot leak it --
  // which in a test about descriptor exhaustion would poison whatever ran next.
  UniqueFd waiting(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(waiting);

  DescriptorLimit limit;
  ASSERT_TRUE(limit.exhaust());
  const bool connected = connectTo(waiting.get(), listener.port());

  out_of_descriptors = false;
  UniqueFd refused = listener.accept(&out_of_descriptors);
  limit.restore();

  ASSERT_TRUE(connected);
  EXPECT_FALSE(refused);
  EXPECT_TRUE(out_of_descriptors)
      << "accept() out of descriptors was reported as an empty backlog, which "
         "under level-triggered epoll is an unbreakable spin";
}

// 31 -- the defect this fixes. A failed accept() leaves the connection in the
// backlog, so the level-triggered listener reports itself readable on every
// turn of the loop. The counter is the observable: the spin drives it up by
// millions in a fraction of a second, the throttle by one a tick. A bound on it
// says the same thing on a fast machine and a loaded one, which a bound on CPU
// time would not.
TEST(FaultInjection, AThrottledLoopKeepsServingAndDoesNotSpin) {
  Server server;

  Client probe(server.port());
  probe.send("PING\r\n");
  ASSERT_EQ(probe.receive(7), "+PONG\r\n");

  UniqueFd waiting(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(waiting);

  DescriptorLimit limit;
  ASSERT_TRUE(limit.exhaust());

  const bool connected = connectTo(waiting.get(), server.port());
  std::this_thread::sleep_for(100ms);

  // INFO over the connection that already exists: opening another is exactly
  // what cannot be done here.
  const long long first = fieldOf(infoOf(probe), "loop0_accept_failures");
  std::this_thread::sleep_for(300ms);
  const long long second = fieldOf(infoOf(probe), "loop0_accept_failures");

  probe.send("PING\r\n");
  const std::string still_alive = probe.receive(7);

  limit.restore();

  ASSERT_TRUE(connected);
  EXPECT_EQ(still_alive, "+PONG\r\n") << "the loop stopped serving its own connections";
  EXPECT_GT(first, 0) << "the accept never failed, so this case proved nothing";
  EXPECT_LT(second - first, 50)
      << "accept failures climbed by " << (second - first)
      << " in 300ms: the listener is being retried on every turn of the loop";
}

// 32 -- the throttle re-arms on a tick rather than latching off.
TEST(FaultInjection, DescriptorsFreedServiceResumes) {
  Server server;

  Client probe(server.port());
  probe.send("PING\r\n");
  ASSERT_EQ(probe.receive(7), "+PONG\r\n");

  UniqueFd waiting(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(waiting);

  {
    DescriptorLimit limit;
    ASSERT_TRUE(limit.exhaust());
    ASSERT_TRUE(connectTo(waiting.get(), server.port()));
    std::this_thread::sleep_for(200ms);  // long enough to be refused a few times
  }

  // The limit is back. The waiting connection must now be accepted and served
  // without anything else prompting the loop.
  const std::string ping = "PING\r\n";
  ASSERT_EQ(::send(waiting.get(), ping.data(), ping.size(), 0),
            static_cast<ssize_t>(ping.size()));

  timeval timeout{2, 0};
  ::setsockopt(waiting.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char reply[8] = {};
  const ssize_t got = ::recv(waiting.get(), reply, 7, 0);

  EXPECT_EQ(got, 7) << "the connection that waited was never accepted: the "
                       "throttle latched off instead of re-arming";
  EXPECT_EQ(std::string(reply, got > 0 ? static_cast<std::size_t>(got) : 0u),
            "+PONG\r\n");
}

// 33 -- the aggregate's width comes from InfoField::kCount, so a field added in
// one place and not another misaligns every later field rather than failing to
// compile. This is what catches that.
TEST(Integration, InfoCarriesTheNewCountersAtEveryShardCount) {
  Server server;
  Client client(server.port());
  const std::string info = infoOf(client);

  EXPECT_GE(fieldOf(info, "loop0_read_pauses"), 0)
      << "loop0_read_pauses is missing from INFO";
  EXPECT_GE(fieldOf(info, "loop0_accept_failures"), 0)
      << "loop0_accept_failures is missing from INFO";

  // The existing fields must still read correctly beside them: a misaligned
  // aggregate reports one loop's number under another's name.
  EXPECT_GE(fieldOf(info, "loop0_connections"), 1);
  EXPECT_GE(fieldOf(info, "loop0_short_writes"), 0);
  EXPECT_GE(fieldOf(info, "loop0_peer_gone_writes"), 0);
  EXPECT_GE(fieldOf(info, "loop0_cross_shard_requests"), 0);
  EXPECT_GE(fieldOf(info, "shard0_keys"), 0);
}
