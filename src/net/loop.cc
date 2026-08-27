#include "net/loop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "net/connection.h"
#include "net/listener.h"

namespace shardkv {
namespace {

constexpr int kMaxEvents = 64;

[[noreturn]] void fail(const char* what) {
  throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

}  // namespace

Loop::Loop(std::uint16_t port, Shard& shard) : shard_(&shard) {
  epoll_fd_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd_) fail("epoll_create1");

  listener_ = std::make_unique<Listener>(port);
  port_ = listener_->port();

  wake_fd_ = UniqueFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!wake_fd_) fail("eventfd");

  epoll_event ev{};
  ev.events = EPOLLIN;

  // data carries the fd, never a pointer -- see the note in loop.h.
  ev.data.fd = listener_->fd();
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, listener_->fd(), &ev) < 0) {
    fail("epoll_ctl(listener)");
  }

  ev.data.fd = wake_fd_.get();
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, wake_fd_.get(), &ev) < 0) {
    fail("epoll_ctl(eventfd)");
  }
}

Loop::~Loop() = default;

std::uint16_t Loop::port() const { return port_; }

void Loop::stop() {
  // The flag is set BEFORE the wakeup, and the order is the whole point.
  //
  // The other way round loses the stop: the loop wakes on the eventfd, drains
  // it, comes back to the top, reads stopping_ while it is still false, and
  // re-enters epoll_wait with no timeout. Nothing will ever wake it again, and
  // join() hangs forever. Setting the flag first means a loop that reaches the
  // top after the wakeup sees the flag, and one that is still inside
  // epoll_wait is woken by the write.
  stopping_.store(true, std::memory_order_release);

  const std::uint64_t one = 1;
  ssize_t written = 0;
  do {
    written = ::write(wake_fd_.get(), &one, sizeof(one));
  } while (written < 0 && errno == EINTR);
}

void Loop::run() {
  std::vector<epoll_event> events(kMaxEvents);

  while (!stopping_.load(std::memory_order_acquire)) {
    const int n = ::epoll_wait(epoll_fd_.get(), events.data(),
                               static_cast<int>(events.size()), -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      fail("epoll_wait");
    }

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;

      if (fd == wake_fd_.get()) {
        std::uint64_t drained = 0;
        while (::read(wake_fd_.get(), &drained, sizeof(drained)) > 0) {
        }
        continue;
      }

      if (fd == listener_->fd()) {
        acceptReady();
        continue;
      }

      // A lookup that misses is ignored rather than asserted on. Two reasons,
      // both real: an earlier event in this same batch may have closed this
      // connection, and the kernel reuses descriptor numbers.
      const auto it = connections_.find(fd);
      if (it == connections_.end()) continue;

      Connection& connection = *it->second;
      bool alive = true;
      if ((events[i].events & (EPOLLHUP | EPOLLERR)) != 0) {
        alive = false;
      } else {
        if ((events[i].events & EPOLLIN) != 0) alive = connection.onReadable();
        if (alive && (events[i].events & EPOLLOUT) != 0) alive = connection.onWritable();
      }

      if (!alive) {
        closeConnection(fd);
        continue;
      }
      updateInterest(connection);
    }
  }
}

void Loop::acceptReady() {
  // Drain the backlog: the listener is level-triggered, but taking every
  // pending connection now costs one extra accept() and saves a wakeup per
  // connection under a burst.
  for (;;) {
    UniqueFd client = listener_->accept();
    if (!client) return;

    const int fd = client.get();
    auto connection = std::make_unique<Connection>(std::move(client), *shard_, stats_);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
      continue;  // connection object is destroyed here, closing the fd
    }
    connections_.emplace(fd, std::move(connection));
    stats_.connections = connections_.size();
  }
}

// EPOLLOUT is registered only while a write is outstanding. Holding it
// permanently would spin the loop: under level-triggered epoll a writable
// socket reports itself writable every time round.
void Loop::updateInterest(Connection& connection) {
  const bool wants_write = connection.wantsWrite();
  if (wants_write == connection.registeredForWrite()) return;

  epoll_event ev{};
  ev.events = EPOLLIN | (wants_write ? EPOLLOUT : 0u);
  ev.data.fd = connection.fd();
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, connection.fd(), &ev) == 0) {
    connection.setRegisteredForWrite(wants_write);
  }
}

void Loop::closeConnection(int fd) {
  // EPOLL_CTL_DEL before the descriptor is closed. The other order leaves a
  // closed fd registered in the epoll set.
  ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
  connections_.erase(fd);  // ~Connection closes it, via UniqueFd
  stats_.connections = connections_.size();
}

}  // namespace shardkv
