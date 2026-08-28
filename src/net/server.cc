#include "net/server.h"

#include <pthread.h>
#include <sched.h>
#include <sys/sysinfo.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "net/loop.h"

namespace shardkv {

std::optional<std::size_t> parseShardCountForTest(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::size_t value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  // The whole argument must be consumed: "4x" is a mistake, not a 4. A
  // benchmark launched with a typo must not quietly measure something else.
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  if (value == 0) return std::nullopt;
  return value;
}

std::size_t defaultShardCount() {
  const unsigned cores = std::thread::hardware_concurrency();
  return cores == 0 ? 1 : static_cast<std::size_t>(cores);
}

Server::Server(Options options, const Clock& clock)
    : options_(options), clock_(&clock) {
  if (options_.shards == 0) options_.shards = 1;

  table_.inboxes.reserve(options_.shards);
  for (std::size_t i = 0; i < options_.shards; ++i) {
    table_.inboxes.push_back(std::make_unique<Inbox>());
    shards_.push_back(std::make_unique<Shard>(clock));
  }

  // Loop 0 binds first so the kernel can assign it a port; the rest join that
  // same port through SO_REUSEPORT.
  for (std::size_t i = 0; i < options_.shards; ++i) {
    const std::uint16_t port = (i == 0) ? options_.port : port_;
    loops_.push_back(
        std::make_unique<Loop>(port, *shards_[i], i, table_, options_.pin));
    if (i == 0) port_ = loops_[0]->port();
  }
}

Server::~Server() { stop(); }

std::uint16_t Server::port() const { return port_; }

std::size_t Server::shardCount() const { return options_.shards; }

void Server::start() {
  // Threads only. Affinity is each loop's own business, applied from its own
  // thread in Loop::run(): doing it here meant the server thread writing a
  // field that loop threads read, which is a race for the sake of a boolean.
  for (std::size_t i = 0; i < loops_.size(); ++i) {
    threads_.emplace_back([this, i] { loops_[i]->run(); });
  }
}

// Two-phase, and the order is what makes it safe.
//
// Stopping loops one at a time and letting each drain its own inbox leaks
// exactly what it protects: a loop that has already exited can still be sent to
// by one that has not, and those nodes are never freed. So production stops
// everywhere first, then consumption, and only when no thread is left does
// anything drain.
void Server::stop() {
  if (stopped_) return;
  stopped_ = true;

  // 1. No loop produces new cross-shard work from here on. LoopRouter::send and
  //    the reply path both check this before enqueueing.
  table_.stopping.store(true, std::memory_order_release);

  // 2. Wake every loop out of epoll_wait and let it leave.
  for (auto& loop : loops_) loop->stop();
  for (auto& thread : threads_) {
    if (thread.joinable()) thread.join();
  }
  threads_.clear();

  // 3. Now, with no producer and no consumer running, whatever is left in the
  //    inboxes is nobody's but ours. The queue destructors drain and free it;
  //    doing it here rather than earlier is the whole point of the ordering.
  loops_.clear();
  table_.inboxes.clear();
}

}  // namespace shardkv
