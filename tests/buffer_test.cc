// Test cases 1-6 from task.md.
//
// A Buffer is a string plus an offset saying how much has been consumed. Until
// the resource-management task it reset only when it drained completely, so any
// unread byte behind the offset kept the whole consumed prefix alive.
//
// Cases 2 and 3 pin the new behaviour: the prefix is reclaimed, and the
// capacity is not. Cases 1, 4, 5 and 6 pin properties compaction must not
// break, and are here because moving bytes under a reader is exactly how this
// change goes wrong.

#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "base/buffer.h"

using shardkv::Buffer;

namespace {

constexpr std::size_t kKiB = 1024;

// A pattern that makes a misplaced byte obvious: every position holds a
// different value from its neighbours, so a tail moved to the wrong offset does
// not read back as the right bytes by luck.
std::string pattern(std::size_t n, unsigned seed = 0) {
  std::string s;
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    s.push_back(static_cast<char>('A' + ((i * 7 + seed) % 26)));
  }
  return s;
}

}  // namespace

// 1 -- the byte floor. A connection that consumes a few dozen bytes at a time
// must not pay a memmove for it.
TEST(BufferTest, ASmallConsumedPrefixIsNotCompacted) {
  Buffer buffer;
  const std::string bytes = pattern(4 * kKiB);
  buffer.append(bytes);

  const char* before = buffer.readable().data();
  buffer.consume(1 * kKiB);

  EXPECT_EQ(buffer.size(), 3 * kKiB);
  EXPECT_EQ(buffer.readable(), std::string_view(bytes).substr(1 * kKiB));
  EXPECT_EQ(buffer.readable().data(), before + 1 * kKiB)
      << "a 1 KiB prefix is below the compaction floor; nothing should have moved";
}

// 2 -- the prefix is reclaimed while unread bytes are still behind it. This is
// the whole feature.
TEST(BufferTest, ALargeConsumedPrefixIsCompactedInPlace) {
  Buffer buffer;
  const std::string bytes = pattern(64 * kKiB);
  buffer.append(bytes);

  // Nothing has been consumed yet, so this is where the allocation begins.
  const char* base = buffer.readable().data();
  buffer.consume(32 * kKiB);

  EXPECT_EQ(buffer.size(), 32 * kKiB);
  EXPECT_EQ(buffer.readable(), std::string_view(bytes).substr(32 * kKiB))
      << "the unread tail must survive the move byte for byte";
  EXPECT_EQ(buffer.readable().data(), base)
      << "the tail was not moved down: the consumed prefix is still held";
}

// 3 -- pins the stage-2 decision. "Compact" must never quietly become "shrink":
// the peak allocation is kept and reused deliberately.
TEST(BufferTest, CompactionDoesNotShrinkCapacity) {
  Buffer buffer;
  buffer.append(pattern(64 * kKiB));
  buffer.consume(32 * kKiB);

  EXPECT_GE(buffer.capacity(), 64 * kKiB)
      << "compaction returned memory to the allocator; it is not supposed to";
}

// 4 -- the request-at-a-time case, which drains completely every cycle and must
// stay flat however long the connection lives.
TEST(BufferTest, SteadySmallTrafficKeepsTheBufferBounded) {
  Buffer buffer;
  const std::string chunk = pattern(64);

  for (int i = 0; i < 100000; ++i) {
    buffer.append(chunk);
    buffer.consume(chunk.size());
  }

  EXPECT_TRUE(buffer.empty());
  EXPECT_LT(buffer.capacity(), 64 * kKiB);
}

// 5 -- a burst drained in steps. Every byte written comes back out, in order,
// across however many compactions happen on the way.
TEST(BufferTest, ABurstDrainsByteForByte) {
  Buffer buffer;
  const std::string bytes = pattern(1024 * kKiB);
  buffer.append(bytes);

  std::string drained;
  std::size_t remaining = bytes.size();
  while (remaining > 0) {
    const std::size_t step = std::min<std::size_t>(64 * kKiB, remaining);
    ASSERT_EQ(buffer.size(), remaining);
    drained.append(buffer.readable().substr(0, step));
    buffer.consume(step);
    remaining -= step;
  }

  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(drained, bytes);
}

// 6 -- the bug compaction can have: appending onto a tail that has just been
// moved, so that the join between the old bytes and the new ones is wrong.
TEST(BufferTest, AppendingAfterACompactionCorruptsNeitherPart) {
  Buffer buffer;
  const std::string a = pattern(16 * kKiB, /*seed=*/0);
  const std::string b = pattern(16 * kKiB, /*seed=*/13);

  buffer.append(a);
  buffer.consume(12 * kKiB);  // over the floor and over half: compacts
  buffer.append(b);

  const std::string expected = a.substr(12 * kKiB) + b;
  ASSERT_EQ(buffer.size(), expected.size());

  std::string drained(buffer.readable());
  buffer.consume(drained.size());

  EXPECT_EQ(drained, expected);
  EXPECT_TRUE(buffer.empty());
}
