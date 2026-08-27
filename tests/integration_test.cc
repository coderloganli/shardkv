// Test cases 95-102 from task.md.
//
// A real Loop on a real port, driven by real sockets. The loop binds port 0 so
// the kernel picks a free one and concurrent runs do not collide.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "helpers.h"
#include "net/connection.h"
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
    loop_ = std::make_unique<Loop>(0, shard_);
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

  // Reads until exactly n bytes have arrived, or the peer closes.
  std::string receive(std::size_t n) {
    std::string out;
    char buf[4096];
    while (out.size() < n) {
      const ssize_t got = ::recv(fd_, buf, sizeof(buf), 0);
      if (got <= 0) break;
      out.append(buf, static_cast<std::size_t>(got));
    }
    return out;
  }

  // Reads whatever is there until the peer closes.
  std::string receiveUntilClosed() {
    std::string out;
    char buf[4096];
    for (;;) {
      const ssize_t got = ::recv(fd_, buf, sizeof(buf), 0);
      if (got <= 0) break;
      out.append(buf, static_cast<std::size_t>(got));
    }
    return out;
  }

  // Reads until the marker appears, for replies whose length is not known in
  // advance.
  std::string receiveUntilContains(std::string_view marker) {
    std::string out;
    char buf[4096];
    for (int i = 0; i < 100 && out.find(marker) == std::string::npos; ++i) {
      const ssize_t got = ::recv(fd_, buf, sizeof(buf), 0);
      if (got <= 0) break;
      out.append(buf, static_cast<std::size_t>(got));
    }
    return out;
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
  int fd_ = -1;
};

// Reads one numeric field out of INFO, over a connection of its own. Returns
// -1 if the field is absent, so a caller's comparison fails loudly.
long long infoField(std::uint16_t port, const std::string& name) {
  Client probe(port);
  probe.send("INFO\r\n");
  const std::string info = probe.receiveUntilContains(name + ":");
  const auto at = info.find(name + ":");
  if (at == std::string::npos) return -1;
  const auto start = at + name.size() + 1;
  const auto eol = info.find("\r\n", start);
  if (eol == std::string::npos) return -1;
  return std::stoll(info.substr(start, eol - start));
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

  first.send("INFO\r\n");
  std::string reply = first.receiveUntilContains("loop0_connections:");
  EXPECT_NE(reply.find("loop0_connections:1\r\n"), std::string::npos) << reply;

  {
    Client second(server.port());
    second.send("PING\r\n");
    ASSERT_EQ(second.receive(7), "+PONG\r\n");  // ensure it is accepted first

    first.send("INFO\r\n");
    reply = first.receiveUntilContains("loop0_connections:");
    EXPECT_NE(reply.find("loop0_connections:2\r\n"), std::string::npos) << reply;
  }

  std::this_thread::sleep_for(200ms);  // let the loop reap the closed one
  first.send("INFO\r\n");
  reply = first.receiveUntilContains("loop0_connections:");
  EXPECT_NE(reply.find("loop0_connections:1\r\n"), std::string::npos) << reply;
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
  Connection connection(UniqueFd(fds[0]), shard, stats);

  // Reads the queued command, answers it, and writes to the dead peer.
  EXPECT_FALSE(connection.onReadable()) << "the connection should report itself finished";
  EXPECT_EQ(stats.peer_gone_writes, 1u)
      << "the write to the departed peer was never attempted";
}
