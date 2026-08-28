// Test cases 12-24 from task.md: key-to-shard mapping, and the reply slots.
//
// Pure unit tests. No threads, no sockets.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "base/buffer.h"
#include "net/reply_slots.h"
#include "store/hash.h"

using namespace shardkv;

namespace {

std::string flushed(ReplySlots& slots) {
  Buffer out;
  slots.takeReadyPrefix(out);
  return std::string(out.readable());
}

}  // namespace

// ------------------------------------------------ 12-15 key to shard

// 12
TEST(ShardMapping, ShardForKeyIsStableAcrossCalls) {
  const std::size_t first = shardForKey("user:42", 8);
  for (int i = 0; i < 100; ++i) EXPECT_EQ(shardForKey("user:42", 8), first);
}

// 13
TEST(ShardMapping, ShardForKeyStaysInRange) {
  for (std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    for (int i = 0; i < 10000; ++i) {
      const std::size_t shard = shardForKey("key:" + std::to_string(i), n);
      EXPECT_LT(shard, n) << "key " << i << " with " << n << " shards";
    }
  }
}

// 14 -- this is the point of ADR 0002. A hash without avalanche piles
// structured names onto a subset of shards, and the result is not a wrong
// answer: it is a scaling curve that lies.
TEST(ShardMapping, ShardForKeyUsesAllShardsForStructuredKeys) {
  constexpr std::size_t kShards = 8;
  constexpr int kKeys = 10000;
  std::vector<int> counts(kShards, 0);

  for (int i = 0; i < kKeys; ++i) {
    counts[shardForKey("user:" + std::to_string(i), kShards)]++;
  }

  for (std::size_t s = 0; s < kShards; ++s) {
    EXPECT_GT(counts[s], kKeys / kShards / 2)
        << "shard " << s << " received only " << counts[s] << " of " << kKeys;
  }
}

// 15
TEST(ShardMapping, ShardForKeyWithOneShardIsAlwaysZero) {
  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(shardForKey("anything:" + std::to_string(i), 1), 0u);
  }
}

// ------------------------------------------------------ 16-24 reply slots

// 16
TEST(ReplySlotsTest, ReserveHandsOutIncreasingSlots) {
  ReplySlots slots;
  EXPECT_EQ(slots.reserve(), 0u);
  EXPECT_EQ(slots.reserve(), 1u);
  EXPECT_EQ(slots.reserve(), 2u);
}

// 17
TEST(ReplySlotsTest, FilledInOrderFlushesImmediately) {
  ReplySlots slots;
  const auto a = slots.reserve();
  const auto b = slots.reserve();

  slots.fill(a, "+ONE\r\n");
  EXPECT_EQ(flushed(slots), "+ONE\r\n");
  slots.fill(b, "+TWO\r\n");
  EXPECT_EQ(flushed(slots), "+TWO\r\n");
}

// 18 -- the central invariant.
TEST(ReplySlotsTest, FilledOutOfOrderHoldsBackUntilGapCloses) {
  ReplySlots slots;
  const auto a = slots.reserve();
  const auto b = slots.reserve();
  const auto c = slots.reserve();

  slots.fill(c, "+THREE\r\n");
  EXPECT_EQ(flushed(slots), "") << "nothing may go out ahead of slot 0";
  slots.fill(b, "+TWO\r\n");
  EXPECT_EQ(flushed(slots), "") << "slot 0 is still missing";
  slots.fill(a, "+ONE\r\n");
  EXPECT_EQ(flushed(slots), "+ONE\r\n+TWO\r\n+THREE\r\n");
}

// 19
TEST(ReplySlotsTest, PartialPrefixFlushes) {
  ReplySlots slots;
  const auto a = slots.reserve();
  const auto b = slots.reserve();
  const auto c = slots.reserve();

  slots.fill(a, "+ONE\r\n");
  slots.fill(c, "+THREE\r\n");
  EXPECT_EQ(flushed(slots), "+ONE\r\n") << "the flush stops at the gap";

  slots.fill(b, "+TWO\r\n");
  EXPECT_EQ(flushed(slots), "+TWO\r\n+THREE\r\n");
}

// 20 -- "memory does not grow without bound" is not assertable; this is the
// observable postcondition that stands in for it.
TEST(ReplySlotsTest, SlotsAreEmptyAfterEveryPrefixFlush) {
  ReplySlots slots;
  for (int round = 0; round < 10000; ++round) {
    const auto s = slots.reserve();
    slots.fill(s, "+OK\r\n");
    EXPECT_EQ(flushed(slots), "+OK\r\n");
    ASSERT_TRUE(slots.idle()) << "round " << round;
    ASSERT_EQ(slots.pendingForTest(), 0u) << "round " << round;
  }
}

// 21
TEST(ReplySlotsTest, IdleIsFalseWhileASlotIsOutstanding) {
  ReplySlots slots;
  EXPECT_TRUE(slots.idle());

  const auto s = slots.reserve();
  EXPECT_FALSE(slots.idle());
  EXPECT_EQ(slots.pendingForTest(), 1u);

  slots.fill(s, "+OK\r\n");
  (void)flushed(slots);
  EXPECT_TRUE(slots.idle());
}

// 22
TEST(ReplySlotsTest, AggregateCompletesOnlyWhenAllGroupsArrive) {
  ReplySlots slots;
  const auto s = slots.reserve();

  Aggregate agg;
  agg.kind = AggregateKind::kArray;
  agg.remaining = 3;
  agg.parts.resize(3);
  slots.beginAggregate(s, std::move(agg));

  slots.contribute(s, 0, std::string("a"));
  EXPECT_EQ(flushed(slots), "") << "still waiting on two groups";
  slots.contribute(s, 1, std::string("b"));
  EXPECT_EQ(flushed(slots), "") << "still waiting on one group";
  slots.contribute(s, 2, std::string("c"));
  EXPECT_EQ(flushed(slots), "*3\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
}

// 23 -- the easiest thing in this design to get wrong. Groups come back in
// whatever order the shards were scheduled; the reply is in argument order.
TEST(ReplySlotsTest, AggregateAssemblesByOriginalArgumentOrder) {
  ReplySlots slots;
  const auto s = slots.reserve();

  Aggregate agg;
  agg.kind = AggregateKind::kArray;
  agg.remaining = 3;
  agg.parts.resize(3);
  slots.beginAggregate(s, std::move(agg));

  slots.contribute(s, 2, std::string("third"));
  slots.contribute(s, 0, std::string("first"));
  slots.contribute(s, 1, std::nullopt);  // a missing key

  EXPECT_EQ(flushed(slots), "*3\r\n$5\r\nfirst\r\n$-1\r\n$5\r\nthird\r\n");
}

// 24
TEST(ReplySlotsTest, AggregateCounterKindSumsContributions) {
  ReplySlots slots;
  const auto s = slots.reserve();

  Aggregate agg;
  agg.kind = AggregateKind::kCount;
  agg.remaining = 3;
  slots.beginAggregate(s, std::move(agg));

  slots.contributeCount(s, 2);
  slots.contributeCount(s, 0);
  EXPECT_EQ(flushed(slots), "");
  slots.contributeCount(s, 5);
  EXPECT_EQ(flushed(slots), ":7\r\n");
}
