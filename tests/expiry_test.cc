// Test cases 69-80 from task.md.
//
// Only the lazy half of expiry exists in this task: a key past its deadline is
// dropped when it is next looked up. The sampled half comes later, and case 80
// is the marker for it.
//
// Time moves because the test moves it. Sleeping here would be slow when it
// passes and flaky when it does not.

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "helpers.h"

using namespace shardkv;
using shardkv::testing::Fixture;
using shardkv::testing::run;

using std::chrono::seconds;

// 69
TEST(Expiry, TtlOnMissingKeyIsMinusTwo) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "TTL nosuch"), ":-2\r\n");
}

// 70
TEST(Expiry, TtlOnKeyWithoutExpiryIsMinusOne) {
  Fixture f;
  run(f.shard, "SET k v");
  EXPECT_EQ(run(f.shard, "TTL k"), ":-1\r\n");
}

// 71 -- the clock has not moved, so this is an exact value, not a range.
TEST(Expiry, ExpireThenTtlReturnsRemaining) {
  Fixture f;
  run(f.shard, "SET k v");
  EXPECT_EQ(run(f.shard, "EXPIRE k 100"), ":1\r\n");
  EXPECT_EQ(run(f.shard, "TTL k"), ":100\r\n");
}

// 72
TEST(Expiry, ExpireOnMissingKeyReturnsZero) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "EXPIRE nosuch 100"), ":0\r\n");
}

// 73
TEST(Expiry, SetWithExSetsTtl) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "SET k v EX 100"), "+OK\r\n");
  EXPECT_EQ(run(f.shard, "TTL k"), ":100\r\n");
}

// 74 -- and the key must not have been written before the argument was
// rejected.
TEST(Expiry, SetWithExZeroIsError) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "SET k v EX 0"),
            "-ERR invalid expire time in 'set' command\r\n");
  EXPECT_EQ(run(f.shard, "GET k"), "$-1\r\n");
}

// 75
TEST(Expiry, SetWithExNegativeIsError) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "SET k v EX -1"),
            "-ERR invalid expire time in 'set' command\r\n");
  EXPECT_EQ(run(f.shard, "GET k"), "$-1\r\n");
}

// 76 -- no command may observe an expired key.
TEST(Expiry, ExpiredKeyReadsAsMissing) {
  Fixture f;
  run(f.shard, "SET k v EX 100");
  f.clock.advance(seconds(101));
  EXPECT_EQ(run(f.shard, "GET k"), "$-1\r\n");
  EXPECT_EQ(run(f.shard, "EXISTS k"), ":0\r\n");
  EXPECT_EQ(run(f.shard, "TTL k"), ":-2\r\n");
}

// 77 -- erased on lookup, not merely hidden.
TEST(Expiry, ExpiredKeyIsRemovedOnLookup) {
  Fixture f;
  run(f.shard, "SET k v EX 100");
  f.clock.advance(seconds(101));
  run(f.shard, "GET k");
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":0\r\n");
}

// 78
TEST(Expiry, PersistRemovesTtl) {
  Fixture f;
  run(f.shard, "SET k v EX 100");
  EXPECT_EQ(run(f.shard, "PERSIST k"), ":1\r\n");
  EXPECT_EQ(run(f.shard, "TTL k"), ":-1\r\n");
}

// 79
TEST(Expiry, PersistOnKeyWithoutTtlReturnsZero) {
  Fixture f;
  run(f.shard, "SET k v");
  EXPECT_EQ(run(f.shard, "PERSIST k"), ":0\r\n");
}

// 80 -- THIS PINS A KNOWN GAP, NOT DESIRED BEHAVIOUR.
//
// With only lazy expiry, a key that expires and is never looked at again is
// never freed, and DBSIZE still counts it. When sampled expiry lands in the
// resource-management task this test will fail, and the right response then is
// to change this test -- not to delete it, and not to weaken the sampling.
TEST(Expiry, DbsizeCountsExpiredKeyNeverLookedUp) {
  Fixture f;
  run(f.shard, "SET k v EX 100");
  f.clock.advance(seconds(101));
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":1\r\n");
}

// Added during stage 7, not from the original list: manual verification against
// a real redis-cli showed TTL answering 99 immediately after SET k v EX 100,
// because the remaining time was truncated to seconds rather than rounded.
// Redis's documented example answers 10 after EXPIRE k 10, so truncation is
// wrong. The manual clock cannot express the sub-second case, so this drives it
// through the shard directly.
TEST(Expiry, TtlRoundsToNearestSecond) {
  Fixture f;
  run(f.shard, "SET k v EX 100");

  // 1ms in: 99.999s remaining. Truncation would answer 99.
  f.clock.advance(std::chrono::milliseconds(1));
  EXPECT_EQ(run(f.shard, "TTL k"), ":100\r\n");

  // 600ms in: 99.4s remaining, which rounds down.
  f.clock.advance(std::chrono::milliseconds(599));
  EXPECT_EQ(run(f.shard, "TTL k"), ":99\r\n");
}

// Added in stage 8, after review. A TTL is attacker-supplied, and converting a
// huge one to seconds and adding it to a steady_clock::time_point overflows
// that clock's nanosecond duration -- undefined behaviour, and in practice a
// deadline in the past, which deletes the key the command was asked to keep.
TEST(Expiry, SetWithExHugeValueIsRejected) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "SET k v EX 9223372036854775807"),
            "-ERR invalid expire time in 'set' command\r\n");
  EXPECT_EQ(run(f.shard, "GET k"), "$-1\r\n");
}

TEST(Expiry, ExpireWithHugeValueIsRejected) {
  Fixture f;
  run(f.shard, "SET k v");
  EXPECT_EQ(run(f.shard, "EXPIRE k 9223372036854775807"),
            "-ERR invalid expire time in 'expire' command\r\n");
  // The key survives, and survives without a deadline.
  EXPECT_EQ(run(f.shard, "GET k"), "$1\r\nv\r\n");
  EXPECT_EQ(run(f.shard, "TTL k"), ":-1\r\n");
}

// The bound itself is usable: a hundred years is accepted, one second past it
// is not.
TEST(Expiry, ExpireAcceptsTheLargestAllowedTtl) {
  Fixture f;
  run(f.shard, "SET k v");
  EXPECT_EQ(run(f.shard, "EXPIRE k 3153600000"), ":1\r\n");
  EXPECT_EQ(run(f.shard, "TTL k"), ":3153600000\r\n");
  EXPECT_EQ(run(f.shard, "EXPIRE k 3153600001"),
            "-ERR invalid expire time in 'expire' command\r\n");
}
