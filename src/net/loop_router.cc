#include "net/loop_router.h"

#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <utility>

namespace shardkv {

void wakeIfNeeded(Inbox& inbox, bool was_empty) {
  if (!was_empty) return;
  const std::uint64_t one = 1;
  ssize_t written = 0;
  do {
    written = ::write(inbox.wake_fd, &one, sizeof(one));
  } while (written < 0 && errno == EINTR);
}

void LoopRouter::send(std::size_t shard, CrossShardRequest request) {
  // Once the server is stopping, no loop produces new work. This is what makes
  // the two-phase shutdown safe: every producer stops before any inbox is
  // drained, so nothing can be enqueued to a loop that has already gone.
  if (table_->stopping.load(std::memory_order_acquire)) return;

  ++stats_->cross_shard_requests;

  Inbox& inbox = *table_->inboxes[shard];
  auto* node = new MpscQueue<CrossShardRequest>::Node();
  node->value = std::move(request);
  wakeIfNeeded(inbox, inbox.requests.push(node));
}

}  // namespace shardkv
