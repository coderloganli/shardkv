// Test cases 28-36 from task.md.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "helpers.h"
#include "proto/encoder.h"
#include "proto/parser.h"

using namespace shardkv;
using shardkv::testing::parseOnce;

// 28
TEST(Encoder, EncodesSimpleString) {
  std::string out;
  resp::encodeSimpleString("OK", out);
  EXPECT_EQ(out, "+OK\r\n");
}

// 29
TEST(Encoder, EncodesError) {
  std::string out;
  resp::encodeError("ERR something", out);
  EXPECT_EQ(out, "-ERR something\r\n");
}

// 30
TEST(Encoder, EncodesInteger) {
  std::string negative;
  resp::encodeInteger(-42, negative);
  EXPECT_EQ(negative, ":-42\r\n");

  std::string zero;
  resp::encodeInteger(0, zero);
  EXPECT_EQ(zero, ":0\r\n");
}

// 31
TEST(Encoder, EncodesBulkString) {
  std::string out;
  resp::encodeBulkString("hello", out);
  EXPECT_EQ(out, "$5\r\nhello\r\n");
}

// 32
TEST(Encoder, EncodesEmptyBulkString) {
  std::string out;
  resp::encodeBulkString("", out);
  EXPECT_EQ(out, "$0\r\n\r\n");
}

// 33 -- a missing value is not an empty one, and clients depend on the
// difference.
TEST(Encoder, EncodesNullBulkString) {
  std::string null_out;
  resp::encodeNullBulkString(null_out);
  EXPECT_EQ(null_out, "$-1\r\n");

  std::string empty_out;
  resp::encodeBulkString("", empty_out);
  EXPECT_NE(null_out, empty_out);
}

// 34
TEST(Encoder, EncodesEmptyArray) {
  std::string out;
  resp::encodeArray({}, out);
  EXPECT_EQ(out, "*0\r\n");
}

// 35 -- the shape MGET returns for keys it did not find.
TEST(Encoder, EncodesArrayWithNullElement) {
  std::vector<std::optional<std::string>> elements = {
      std::string("hello"), std::nullopt, std::string("world")};
  std::string out;
  resp::encodeArray(elements, out);
  EXPECT_EQ(out, "*3\r\n$5\r\nhello\r\n$-1\r\n$5\r\nworld\r\n");
}

// 36
TEST(Encoder, RoundTripsThroughParser) {
  std::vector<std::optional<std::string>> elements = {
      std::string("SET"), std::string("foo"), std::string("bar")};
  std::string encoded;
  resp::encodeArray(elements, encoded);

  auto p = parseOnce(encoded);
  ASSERT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 3u);
  EXPECT_EQ(p.argv[0], "SET");
  EXPECT_EQ(p.argv[1], "foo");
  EXPECT_EQ(p.argv[2], "bar");
  EXPECT_EQ(p.consumed, encoded.size());
}
