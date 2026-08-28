#include "net/loop.h"

#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <thread>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "net/connection.h"
#include "net/listener.h"
#include "net/loop_router.h"

namespace shardkv {
namespace {

constexpr int kMaxEvents = 64;

// Packs the answer to a cross-shard request into a reply message and posts it
// to the loop that asked. The counterpart of SlotDeliverer, which is what a
// loop uses when the keys turned out to be its own.
//
// The delivery kind travels with the reply because the originating loop no
// longer knows what command it belonged to.
struct MessageDeliverer {
  Inbox* origin;
  std::uint64_t conn_id;
  std::uint32_t slot;
  Delivery delivery;

  void post(CrossShardReply reply) {
    reply.conn_id = conn_id;
    reply.slot = slot;
    reply.delivery = delivery;
    auto* node = new MpscQueue<CrossShardReply>::Node();
    node->value = std::move(reply);
    wakeIfNeeded(*origin, origin->replies.push(node));
  }

  void whole(std::string resp) {
    CrossShardReply reply;
    reply.payload = std::move(resp);
    post(std::move(reply));
  }
  void part(std::uint32_t index, std::optional<std::string> value) {
    CrossShardReply reply;
    reply.index = index;
    reply.part = std::move(value);
    post(std::move(reply));
  }
  void count(std::int64_t n) {
    CrossShardReply reply;
    reply.count = n;
    post(std::move(reply));
  }
  void status() { post(CrossShardReply{}); }
};

[[noreturn]] void fail(const char* what) {
  throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

}  // namespace

Loop::Loop(std::uint16_t port, Shard& shard, std::size_t index, LoopTable& table,
           bool pin)
    : shard_(&shard), index_(index), table_(&table) {
  pin_requested_ = pin;
  stats_.pinned = false;  // until this loop's own thread proves otherwise
  stats_.shard_count = table.inboxes.size();
  router_ = std::make_unique<LoopRouter>(index, shard, table, stats_);

  epoll_fd_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd_) fail("epoll_create1");

  listener_ = std::make_unique<Listener>(port);
  port_ = listener_->port();

  wake_fd_ = UniqueFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!wake_fd_) fail("eventfd");
  table.inboxes[index]->wake_fd = wake_fd_.get();

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
  // A loop pins its own thread, from its own thread. Having the server do it
  // after start() meant writing stats_.pinned from one thread while INFO read
  // it from another -- a race, and one that could report the requested value
  // rather than the achieved one. Here both the write and every read are on
  // this loop's thread, so there is nothing to synchronise.
  if (pin_requested_) {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    const int cores = std::max(1, get_nprocs());
    CPU_SET(static_cast<int>(index_) % cores, &cpus);
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    stats_.pinned = (rc == 0);
    if (rc != 0) {
      // Refusing is allowed -- a cpuset may simply not be ours to narrow --
      // but claiming success would be worse than the flag not existing.
      std::fprintf(stderr,
                   "shardkv: loop %zu could not set affinity (%s); unpinned\n",
                   index_, std::strerror(rc));
    }
  }

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
        // One read takes the whole counter, however many wakeups coalesced
        // into it -- eventfd(2). What matters is draining the QUEUES to empty
        // afterwards, not counting wakeups: a wakeup may be redundant, and the
        // rule that elides them guarantees it is never missing.
        std::uint64_t drained = 0;
        while (::read(wake_fd_.get(), &drained, sizeof(drained)) > 0) {
        }
        drainInbox();
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
    auto connection =
        std::make_unique<Connection>(std::move(client), *router_, stats_, ++next_conn_id_);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
      continue;  // connection object is destroyed here, closing the fd
    }
    connection->setRegisteredEvents(EPOLLIN);
    connections_.emplace(fd, std::move(connection));
    stats_.connections = connections_.size();
  }
}

// Interest is exactly what the connection currently wants, and no more. Under
// level-triggered epoll anything registered but not consumed reports itself
// ready on every turn, so a stale interest is not a small inefficiency: it is a
// loop that spins.
//
// EPOLLOUT is therefore held only while a write is outstanding, and EPOLLIN
// only while the connection is still reading. After a terminal slot it is not
// -- bytes sent after QUIT are never read -- so leaving EPOLLIN armed would
// spin until the last outstanding reply arrived.
void Loop::updateInterest(Connection& connection) {
  std::uint32_t events = 0;
  if (connection.wantsRead()) events |= EPOLLIN;
  if (connection.wantsWrite()) events |= EPOLLOUT;
  if (events == connection.registeredEvents()) return;

  epoll_event ev{};
  ev.events = events;
  ev.data.fd = connection.fd();
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, connection.fd(), &ev) == 0) {
    connection.setRegisteredEvents(events);
  }
}

// Everything queued for this loop: work other loops want run against this
// shard, and answers to work this loop sent out.
void Loop::drainInbox() {
  Inbox& inbox = *table_->inboxes[index_];

  // Drain until genuinely empty, not merely until pop says nullptr. See the
  // note on MpscQueue::pop: a producer mid-push looks like an empty queue, and
  // going back to epoll_wait there is how a wakeup gets lost.
  for (;;) {
    bool busy = false;
    auto* node = inbox.requests.pop(&busy);
    if (node == nullptr) {
      if (!busy) break;
      std::this_thread::yield();  // the window is a couple of instructions
      continue;
    }
    CrossShardRequest request = std::move(node->value);
    delete node;  // popped means owned; see the queue's ownership contract

    if (table_->stopping.load(std::memory_order_acquire)) continue;

    // Run it here, where the keys live, and post the answer back. The reply
    // travels to the originating loop rather than this one reaching into that
    // loop's slots: a slot belongs to one thread from creation to flush.
    Inbox& origin = *table_->inboxes[request.origin_loop];
    MessageDeliverer deliverer{&origin, request.conn_id, request.slot,
                               request.delivery};

    LoopFacts facts;
    facts.connections = stats_.connections;
    facts.loops = table_->inboxes.size();
    facts.short_writes = stats_.short_writes;
    facts.peer_gone_writes = stats_.peer_gone_writes;
    facts.cross_shard_requests = stats_.cross_shard_requests;
    facts.pinned = stats_.pinned;
    runCrossShardRequest(*shard_, request, facts, deliverer);
  }

  for (;;) {
    bool busy = false;
    auto* node = inbox.replies.pop(&busy);
    if (node == nullptr) {
      if (!busy) break;
      std::this_thread::yield();
      continue;
    }
    CrossShardReply reply = std::move(node->value);
    delete node;

    // Addressed by id, not by pointer. A connection that has gone since the
    // request left has no entry here, and its reply is simply dropped -- which
    // is why a dying connection never has to wait for replies in flight.
    Connection* target = nullptr;
    for (auto& [fd, connection] : connections_) {
      if (connection->id() == reply.conn_id) {
        target = connection.get();
        break;
      }
    }
    if (target == nullptr) continue;

    // The delivery kind rides along with the reply. Filling a slot that holds a
    // half-finished aggregate would overwrite it, and the remaining groups
    // would have nothing to complete -- a hang rather than a wrong answer.
    switch (reply.delivery) {
      case Delivery::kWhole:
        target->slots().fill(reply.slot, std::move(reply.payload));
        break;
      case Delivery::kArrayParts:
      case Delivery::kLoopInfo:
        target->slots().contribute(reply.slot, reply.index, std::move(reply.part));
        break;
      case Delivery::kCount:
        target->slots().contributeCount(reply.slot, reply.count);
        break;
      case Delivery::kStatus:
        target->slots().contributeCount(reply.slot, 0);
        break;
    }

    if (!target->onSlotsChanged()) {
      closeConnection(target->fd());
    } else {
      updateInterest(*target);
    }
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
