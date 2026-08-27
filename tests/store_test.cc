// Test cases 37-68 from task.md.
//
// Behaviour that must match Redis, including the error paths -- those are the
// details that get written wrong without checking against the real thing.

#include <string>

#include <gtest/gtest.h>

#include "helpers.h"

using namespace shardkv;
using shardkv::testing::Fixture;
using shardkv::testing::run;
using shardkv::testing::runArgv;

namespace {
constexpr const char* kNotAnInteger =
    "-ERR value is not an integer or out of range\r\n";
}

// 37 -- a missing key is null, never an empty string.
TEST(Store, GetMissingKeyReturnsNullBulk) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "GET nosuch"), "$-1\r\n");
  EXPECT_NE(run(f.shard, "GET nosuch"), "$0\r\n\r\n");
}

// 38
TEST(Store, SetThenGetReturnsValue) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "SET k v"), "+OK\r\n");
  EXPECT_EQ(run(f.shard, "GET k"), "$1\r\nv\r\n");
}

// 39
TEST(Store, SetOverwritesExistingValue) {
  Fixture f;
  run(f.shard, "SET k a");
  run(f.shard, "SET k bb");
  EXPECT_EQ(run(f.shard, "GET k"), "$2\r\nbb\r\n");
}

// 40 -- values are byte strings (ADR 0004). Storing an integer form would
// return "1" here, and this is where that bug would surface.
TEST(Store, SetPreservesExactBytes) {
  Fixture f;
  run(f.shard, "SET k 001");
  EXPECT_EQ(run(f.shard, "GET k"), "$3\r\n001\r\n");
}

// 41
TEST(Store, SetClearsExistingTtl) {
  Fixture f;
  run(f.shard, "SET k v EX 100");
  run(f.shard, "SET k v2");
  EXPECT_EQ(run(f.shard, "TTL k"), ":-1\r\n");
}

// 42
TEST(Store, IncrOnMissingKeyStartsAtOne) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "INCR c"), ":1\r\n");
}

// 43
TEST(Store, IncrOnExistingValueAdds) {
  Fixture f;
  run(f.shard, "SET c 41");
  EXPECT_EQ(run(f.shard, "INCR c"), ":42\r\n");
  EXPECT_EQ(run(f.shard, "GET c"), "$2\r\n42\r\n");
}

// 44
TEST(Store, IncrOnNonNumericValueIsError) {
  Fixture f;
  run(f.shard, "SET k abc");
  EXPECT_EQ(run(f.shard, "INCR k"), kNotAnInteger);
}

// 45 -- Redis refuses a non-canonical number as a counter.
TEST(Store, IncrOnNonCanonicalNumberIsError) {
  Fixture f;
  run(f.shard, "SET k 001");
  EXPECT_EQ(run(f.shard, "INCR k"), kNotAnInteger);
}

// 46 -- must not wrap around.
TEST(Store, IncrOverflowIsError) {
  Fixture f;
  run(f.shard, "SET c 9223372036854775807");
  EXPECT_EQ(run(f.shard, "INCR c"), kNotAnInteger);
  EXPECT_EQ(run(f.shard, "GET c"), "$19\r\n9223372036854775807\r\n");
}

// 47
TEST(Store, DecrOnMissingKeyIsMinusOne) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "DECR c"), ":-1\r\n");
}

// 48
TEST(Store, DecrUnderflowIsError) {
  Fixture f;
  run(f.shard, "SET c -9223372036854775808");
  EXPECT_EQ(run(f.shard, "DECR c"), kNotAnInteger);
  EXPECT_EQ(run(f.shard, "GET c"), "$20\r\n-9223372036854775808\r\n");
}

// 49
TEST(Store, IncrbyAddsDelta) {
  Fixture f;
  run(f.shard, "SET c 10");
  EXPECT_EQ(run(f.shard, "INCRBY c 5"), ":15\r\n");
}

// 50
TEST(Store, IncrbyNegativeSubtracts) {
  Fixture f;
  run(f.shard, "SET c 10");
  EXPECT_EQ(run(f.shard, "INCRBY c -5"), ":5\r\n");
}

// 51
TEST(Store, DecrbySubtractsDelta) {
  Fixture f;
  run(f.shard, "SET c 10");
  EXPECT_EQ(run(f.shard, "DECRBY c 3"), ":7\r\n");
}

// 52
TEST(Store, DecrbyNegativeAdds) {
  Fixture f;
  run(f.shard, "SET c 10");
  EXPECT_EQ(run(f.shard, "DECRBY c -3"), ":13\r\n");
}

// 53
TEST(Store, IncrbyNonNumericDeltaIsError) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "INCRBY c abc"), kNotAnInteger);
}

// 54
TEST(Store, DelReturnsCountDeleted) {
  Fixture f;
  run(f.shard, "SET a 1");
  run(f.shard, "SET b 2");
  EXPECT_EQ(run(f.shard, "DEL a b nosuch"), ":2\r\n");
}

// 55 -- Redis counts occurrences, it does not deduplicate.
TEST(Store, ExistsCountsRepeats) {
  Fixture f;
  run(f.shard, "SET k v");
  EXPECT_EQ(run(f.shard, "EXISTS k k"), ":2\r\n");
}

// 56
TEST(Store, TypeReturnsStringOrNone) {
  Fixture f;
  run(f.shard, "SET k v");
  EXPECT_EQ(run(f.shard, "TYPE k"), "+string\r\n");
  EXPECT_EQ(run(f.shard, "TYPE nosuch"), "+none\r\n");
}

// 57
TEST(Store, AppendToExistingReturnsNewLength) {
  Fixture f;
  run(f.shard, "SET k foo");
  EXPECT_EQ(run(f.shard, "APPEND k bar"), ":6\r\n");
  EXPECT_EQ(run(f.shard, "GET k"), "$6\r\nfoobar\r\n");
}

// 58
TEST(Store, AppendToMissingKeyCreatesIt) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "APPEND k bar"), ":3\r\n");
  EXPECT_EQ(run(f.shard, "GET k"), "$3\r\nbar\r\n");
}

// 59
TEST(Store, StrlenReturnsLength) {
  Fixture f;
  run(f.shard, "SET k hello");
  EXPECT_EQ(run(f.shard, "STRLEN k"), ":5\r\n");
}

// 60
TEST(Store, StrlenOnMissingKeyIsZero) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "STRLEN nosuch"), ":0\r\n");
}

// 61
TEST(Store, GetsetOnMissingKeyReturnsNull) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "GETSET k v"), "$-1\r\n");
  EXPECT_EQ(run(f.shard, "GET k"), "$1\r\nv\r\n");
}

// 62
TEST(Store, GetsetReturnsPreviousValue) {
  Fixture f;
  run(f.shard, "SET k old");
  EXPECT_EQ(run(f.shard, "GETSET k new"), "$3\r\nold\r\n");
  EXPECT_EQ(run(f.shard, "GET k"), "$3\r\nnew\r\n");
}

// 63
TEST(Store, DbsizeCountsKeys) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":0\r\n");
  run(f.shard, "SET a 1");
  run(f.shard, "SET b 2");
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":2\r\n");
}

// 64
TEST(Store, FlushdbEmptiesShard) {
  Fixture f;
  run(f.shard, "SET a 1");
  run(f.shard, "SET b 2");
  EXPECT_EQ(run(f.shard, "FLUSHDB"), "+OK\r\n");
  EXPECT_EQ(run(f.shard, "DBSIZE"), ":0\r\n");
  EXPECT_EQ(run(f.shard, "GET a"), "$-1\r\n");
}

// 65
TEST(Store, MgetReturnsNullForMissingElements) {
  Fixture f;
  run(f.shard, "SET a 1");
  run(f.shard, "SET b 2");
  EXPECT_EQ(run(f.shard, "MGET a nosuch b"),
            "*3\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n");
}

// 66
TEST(Store, MsetSetsAllAndReturnsOk) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "MSET a 1 b 2"), "+OK\r\n");
  EXPECT_EQ(run(f.shard, "GET a"), "$1\r\n1\r\n");
  EXPECT_EQ(run(f.shard, "GET b"), "$1\r\n2\r\n");
}

// 67 -- and it must not have written the pairs it did see before noticing.
TEST(Store, MsetWithOddArgumentsIsError) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "MSET a 1 b"),
            "-ERR wrong number of arguments for 'mset' command\r\n");
  EXPECT_EQ(run(f.shard, "GET a"), "$-1\r\n");
}

// 68 -- the field names are fixed here so the test and the implementation
// cannot quietly disagree about them.
TEST(Store, InfoReportsShardAndConnectionCounts) {
  Fixture f;
  run(f.shard, "SET a 1");
  const std::string reply = run(f.shard, "INFO");

  ASSERT_FALSE(reply.empty());
  EXPECT_EQ(reply[0], '$') << "INFO replies with a bulk string";
  EXPECT_NE(reply.find("shard0_keys:1\r\n"), std::string::npos) << reply;

  const auto conns = reply.find("loop0_connections:");
  ASSERT_NE(conns, std::string::npos) << reply;
  const auto eol = reply.find("\r\n", conns);
  ASSERT_NE(eol, std::string::npos);
  const std::string field = reply.substr(conns, eol - conns);
  const std::string value = field.substr(field.find(':') + 1);
  ASSERT_FALSE(value.empty());
  EXPECT_NO_THROW((void)std::stoll(value)) << field;
}
