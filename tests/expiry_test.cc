// Test cases 69-80 from the expiry task, and 16-26 from the
// resource-management task.
//
// Both halves of expiry now live here. The lazy half drops a key past its
// deadline when it is next looked up; the sampled half reaps the keys nobody
// looks up again. Case 80 was the marker for the gap between them and has
// become case 16, which asserts that it is closed.
//
// Time moves because the test moves it. Sleeping here would be slow when it
// passes and flaky when it does not -- and that goes double for the sampler,
// which would otherwise be tested by waiting and hoping.

#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "helpers.h"

using namespace shardkv;
using shardkv::testing::Fixture;
using shardkv::testing::run;
using shardkv::testing::runArgv;

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

// 16 -- what case 80 became.
//
// Case 80 pinned the gap: with only lazy expiry, a key that expired and was
// never looked at again was never freed and DBSIZE still counted it. Its own
// comment said that when sampled expiry landed this test would fail, and that
// the right response would be to change the test rather than delete it or
// weaken the sampling. This is that change.
//
// Both halves are still asserted, because the first is what the second is
// about: DBSIZE does not sweep, and the sampler is what makes it converge.
TEST(Expiry, SampledExpiryFreesAKeyNeverLookedUp) {
  Fixture f;
  run(f.shard, "SET k v EX 100");
  f.clock.advance(seconds(101));

  EXPECT_EQ(run(f.shard, "DBSIZE"), ":1\r\n")
      << "DBSIZE does not sweep; nothing has run yet";

  f.shard.sampleExpired(20);

  EXPECT_EQ(run(f.shard, "DBSIZE"), ":0\r\n")
      << "the sampler did not reap a key that nobody looked up";
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

// ---------------------------------------------------------------------------
// Cases 17-26: the sampling pass itself.
//
// Every one of these calls Shard::sampleExpired directly against a ManualClock,
// so nothing here sleeps or depends on scheduling. That the timer actually
// calls it is a separate question, asserted once over a live server in
// tests/integration_test.cc.

namespace {

// Runs enough passes to sweep the whole table several times over.
//
// Deliberately not "until a pass erases nothing": the sampler visits a bounded
// number of keys per pass, so a stretch of live keys makes a pass erase nothing
// while expired keys remain elsewhere. Stopping there would end the sweep on the
// first barren patch and assert against a table that was never fully walked.
void sweep(Fixture& f, std::size_t limit, std::size_t keys) {
  const std::size_t laps = 5;
  const std::size_t passes = (keys / limit + 2) * laps;
  for (std::size_t i = 0; i < passes; ++i) f.shard.sampleExpired(limit);
}

std::string keyOf(int i) { return "key" + std::to_string(i); }

}  // namespace

// 17
TEST(Expiry, SamplingDoesNotTouchALiveKey) {
  Fixture f;
  run(f.shard, "SET a v EX 100");
  run(f.shard, "SET b v");
  f.clock.advance(seconds(50));

  f.shard.sampleExpired(20);

  EXPECT_EQ(run(f.shard, "DBSIZE"), ":2\r\n");
  EXPECT_EQ(run(f.shard, "GET a"), "$1\r\nv\r\n");
  EXPECT_EQ(run(f.shard, "GET b"), "$1\r\nv\r\n");
}

// 18 -- a pass that reported nothing would be indistinguishable from one that
// never ran.
TEST(Expiry, APassReportsWhatItDid) {
  {
    Fixture expired;
    for (int i = 0; i < 10; ++i) {
      runArgv(expired.shard, {"SET", keyOf(i), "v", "EX", "100"});
    }
    expired.clock.advance(seconds(101));

    const auto result = expired.shard.sampleExpired(20);
    EXPECT_GT(result.visited, 0u);
    EXPECT_GT(result.erased, 0u);
  }
  {
    Fixture live;
    for (int i = 0; i < 10; ++i) runArgv(live.shard, {"SET", keyOf(i), "v"});

    const auto result = live.shard.sampleExpired(20);
    EXPECT_GT(result.visited, 0u) << "the pass did not look at anything";
    EXPECT_EQ(result.erased, 0u);
  }
}

// 19 -- the bound is what keeps a tick's cost independent of the shard's size.
TEST(Expiry, APassIsBounded) {
  Fixture f;
  for (int i = 0; i < 1000; ++i) {
    runArgv(f.shard, {"SET", keyOf(i), "v", "EX", "100"});
  }
  f.clock.advance(seconds(101));

  const auto result = f.shard.sampleExpired(20);
  EXPECT_LE(result.visited, 20u);
  EXPECT_LE(result.erased, 20u);
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":980\r\n")
      << "one bounded pass erased more than its limit";
}

// 20 -- proves the cursor advances. A sampler that restarted from the front
// every time would erase the same prefix and never reach the rest.
TEST(Expiry, RepeatedPassesClearEverything) {
  Fixture f;
  for (int i = 0; i < 1000; ++i) {
    runArgv(f.shard, {"SET", keyOf(i), "v", "EX", "100"});
  }
  f.clock.advance(seconds(101));

  sweep(f, /*limit=*/20, /*keys=*/1000);
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":0\r\n");
}

// 21 -- a key past the front of its bucket is still reached.
//
// Written as a property rather than as "keys chosen to collide into one
// bucket", which task.md asked for and which Shard's interface does not permit:
// the hash and the bucket count are the container's business and a test cannot
// choose what collides. What it can do is check that the arrangement the case
// needs is actually present, and refuse to run if it is not -- so this can fail
// loudly rather than pass having asserted nothing.
//
// The arrangement: with one visit per pass, a cursor that remembers only a
// bucket index visits each bucket's FIRST entry and then moves on, so anything
// behind the first entry is never reached. Live keys are what make that
// permanent -- if every key were expired, erasing the first would shift the next
// into its place and even a bucket-only cursor would drain the table -- which is
// why half the keys here have no deadline.
//
// The precondition asserted below is the structural half: some bucket holds more
// than one entry. Which of those entries is live and which expired then follows
// from a fixed key set and a fixed hash, so the case is deterministic for a
// given toolchain; it is simply not analytic, and would rather say so than
// pretend.
TEST(Expiry, AKeyPastTheFrontOfItsBucketIsStillReached) {
  Fixture f;
  for (int i = 0; i < 200; ++i) {
    if (i % 2 == 0) {
      runArgv(f.shard, {"SET", keyOf(i), "v", "EX", "100"});
    } else {
      runArgv(f.shard, {"SET", keyOf(i), "v"});
    }
  }
  f.clock.advance(seconds(101));

  ASSERT_GT(f.shard.largestBucketForTest(), 1u)
      << "no bucket holds more than one key, so nothing here sits past the front "
         "of a bucket and this case would prove nothing about the cursor";

  // One visit a pass, and enough passes for several laps of the table.
  for (int i = 0; i < 4000; ++i) f.shard.sampleExpired(1);

  EXPECT_EQ(run(f.shard, "DBSIZE"), ":100\r\n")
      << "an expired key was never visited: the cursor cannot resume inside a "
         "bucket, so a live key ahead of it hides it";
  for (int i = 1; i < 200; i += 2) {
    EXPECT_EQ(runArgv(f.shard, {"GET", keyOf(i)}), "$1\r\nv\r\n")
        << "the sampler took a live key, at i=" << i;
  }
}

// 22 -- a rehash moves elements between buckets under the cursor. The sweep
// must carry on and must never index a bucket that no longer exists.
TEST(Expiry, TheCursorSurvivesARehash) {
  Fixture f;
  for (int i = 0; i < 50; ++i) {
    runArgv(f.shard, {"SET", keyOf(i), "v", "EX", "100"});
  }
  f.clock.advance(seconds(101));

  f.shard.sampleExpired(5);  // leaves the cursor partway through the table

  // Enough insertions to force the bucket count to change under it.
  for (int i = 1000; i < 3000; ++i) runArgv(f.shard, {"SET", keyOf(i), "v"});

  sweep(f, /*limit=*/20, /*keys=*/2050);
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":2000\r\n")
      << "the expired keys were not all reaped after the table rehashed";
}

// 23 -- the sampler and the lazy path must agree about where a deadline comes
// from.
TEST(Expiry, ADeadlineSetByExpireIsReapedToo) {
  Fixture f;
  run(f.shard, "SET k v");
  EXPECT_EQ(run(f.shard, "EXPIRE k 100"), ":1\r\n");
  f.clock.advance(seconds(101));

  f.shard.sampleExpired(20);
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":0\r\n");
}

// 24 -- the reverse: a key whose deadline was removed must be invisible to the
// sampler however long it waits.
TEST(Expiry, PersistProtectsAKeyFromTheSampler) {
  Fixture f;
  run(f.shard, "SET k v EX 100");
  EXPECT_EQ(run(f.shard, "PERSIST k"), ":1\r\n");
  f.clock.advance(seconds(1000));

  f.shard.sampleExpired(20);

  EXPECT_EQ(run(f.shard, "DBSIZE"), ":1\r\n");
  EXPECT_EQ(run(f.shard, "GET k"), "$1\r\nv\r\n");
}

// 25 -- FLUSHDB can leave the cursor past the end of a table that is now empty,
// and the next insertions rebuild it underneath.
TEST(Expiry, ACursorThatOutlivedItsTableDoesNotMisbehave) {
  Fixture f;
  for (int i = 0; i < 100; ++i) {
    runArgv(f.shard, {"SET", keyOf(i), "v", "EX", "100"});
  }
  f.clock.advance(seconds(101));
  f.shard.sampleExpired(20);  // the cursor is now somewhere inside that table

  EXPECT_EQ(run(f.shard, "FLUSHDB"), "+OK\r\n");
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":0\r\n");

  for (int i = 0; i < 40; ++i) {
    if (i % 2 == 0) {
      runArgv(f.shard, {"SET", keyOf(i), "w", "EX", "10"});
    } else {
      runArgv(f.shard, {"SET", keyOf(i), "w"});
    }
  }
  f.clock.advance(seconds(11));

  sweep(f, /*limit=*/1, /*keys=*/40);

  EXPECT_EQ(run(f.shard, "DBSIZE"), ":20\r\n");
  for (int i = 1; i < 40; i += 2) {
    EXPECT_EQ(runArgv(f.shard, {"GET", keyOf(i)}), "$1\r\nw\r\n");
  }
}

// 26
TEST(Expiry, SamplingAnEmptyShardIsANoOp) {
  Fixture f;
  const auto result = f.shard.sampleExpired(20);
  EXPECT_EQ(result.visited, 0u);
  EXPECT_EQ(result.erased, 0u);
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":0\r\n");
}
