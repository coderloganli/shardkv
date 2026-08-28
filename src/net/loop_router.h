#pragma once

#include <cstddef>

#include "commands/router.h"
#include "net/loop.h"

namespace shardkv {

// The production router: this loop's own shard, and a queue to every other.
//
// Note what it does not offer -- any way to reach another shard directly. That
// is the architecture's central rule, and here it is a property of the type
// rather than something a reviewer has to notice.
class LoopRouter final : public ShardRouter {
 public:
  LoopRouter(std::size_t index, Shard& shard, LoopTable& table, LoopStats& stats)
      : index_(index), shard_(&shard), table_(&table), stats_(&stats) {}

  std::size_t shardCount() const override { return table_->inboxes.size(); }
  std::size_t localShard() const override { return index_; }
  Shard& local() override { return *shard_; }

  void send(std::size_t shard, CrossShardRequest request) override;

 private:
  std::size_t index_;
  Shard* shard_ = nullptr;
  LoopTable* table_ = nullptr;
  LoopStats* stats_ = nullptr;
};

// Wakes a loop by writing 1 to its eventfd, but only when `was_empty` says this
// push made the queue non-empty. A consumer that has not drained yet is either
// running or already has a wakeup pending, so the write would be waste.
//
// eventfd(2): a read without EFD_SEMAPHORE returns the counter and resets it to
// zero, and epoll reports the descriptor readable while the counter is above
// zero. So one read drains any number of coalesced wakeups, and a redundant
// wakeup is harmless where a missing one would hang.
void wakeIfNeeded(Inbox& inbox, bool was_empty);

}  // namespace shardkv
