// Test cases 1-27 from task.md.
//
// The parser is a pure function, so all of this is byte sequences in and
// answers out -- no socket, no event loop.

#include <unistd.h>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "helpers.h"
#include "proto/parser.h"
#include "proto/resp.h"

using namespace shardkv;
using shardkv::testing::parseOnce;

namespace {

// Resident set size in kilobytes, for case 27. Reads statm rather than
// mallinfo because the question is whether the process asked the kernel for
// hundreds of megabytes, not how the allocator accounted for it.
long residentKb() {
  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) return -1;
  long total = 0;
  long resident = 0;
  if (std::fscanf(f, "%ld %ld", &total, &resident) != 2) resident = -1;
  std::fclose(f);
  return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

}  // namespace

// ---------------------------------------------------------- 1-4 normal shapes

// 1
TEST(Parser, ParsesArrayOfBulkStrings) {
  const std::string in = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
  auto p = parseOnce(in);
  EXPECT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 3u);
  EXPECT_EQ(p.argv[0], "SET");
  EXPECT_EQ(p.argv[1], "foo");
  EXPECT_EQ(p.argv[2], "bar");
  EXPECT_EQ(p.consumed, in.size());
}

// 2
TEST(Parser, ParsesEmptyBulkString) {
  const std::string in = "*2\r\n$3\r\nGET\r\n$0\r\n\r\n";
  auto p = parseOnce(in);
  EXPECT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 2u);
  EXPECT_EQ(p.argv[0], "GET");
  EXPECT_EQ(p.argv[1], "");
  EXPECT_TRUE(p.argv[1].empty());
}

// 3
TEST(Parser, ParsesBinarySafePayload) {
  // a \r \n b NUL c -- six bytes, three of which would end a line in a
  // text protocol.
  const std::string payload = std::string("a\r\nb") + '\0' + "c";
  ASSERT_EQ(payload.size(), 6u);
  const std::string in = "*2\r\n$3\r\nSET\r\n$6\r\n" + payload + "\r\n";
  auto p = parseOnce(in);
  EXPECT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 2u);
  EXPECT_EQ(p.argv[1], std::string_view(payload));
}

// 4 -- proves the payload is not copied.
TEST(Parser, ArgvPointsIntoInputBuffer) {
  const std::string in = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
  auto p = parseOnce(in);
  ASSERT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 3u);
  const char* begin = in.data();
  const char* end = in.data() + in.size();
  for (const auto& arg : p.argv) {
    EXPECT_GE(arg.data(), begin);
    EXPECT_LE(arg.data() + arg.size(), end);
  }
}

// ------------------------------------------------------- 5-8 partial reads

// 5
TEST(Parser, PartialHeaderNeedsMore) {
  auto p = parseOnce("*3\r\n$3\r\nSE");
  EXPECT_EQ(p.status, ParseStatus::kNeedMore);
  EXPECT_EQ(p.consumed, 0u);
}

// 6
TEST(Parser, PartialBulkBodyNeedsMore) {
  auto p = parseOnce("*1\r\n$5\r\nhel");
  EXPECT_EQ(p.status, ParseStatus::kNeedMore);
  EXPECT_EQ(p.consumed, 0u);
}

// 7 -- the terminator has not arrived; that is not the same as a wrong one.
TEST(Parser, MissingFinalCrlfNeedsMore) {
  auto p = parseOnce("*1\r\n$3\r\nfoo");
  EXPECT_EQ(p.status, ParseStatus::kNeedMore);
  EXPECT_EQ(p.consumed, 0u);
}

// 8
TEST(Parser, ByteAtATimeFeedYieldsSameCommand) {
  const std::string full = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";
  ASSERT_EQ(full.size(), 31u);
  for (std::size_t n = 1; n < full.size(); ++n) {
    auto p = parseOnce(std::string_view(full).substr(0, n));
    EXPECT_EQ(p.status, ParseStatus::kNeedMore) << "at prefix length " << n;
    EXPECT_EQ(p.consumed, 0u) << "at prefix length " << n;
  }
  auto p = parseOnce(full);
  EXPECT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 3u);
  EXPECT_EQ(p.argv[0], "SET");
  EXPECT_EQ(p.argv[1], "foo");
  EXPECT_EQ(p.argv[2], "bar");
  EXPECT_EQ(p.consumed, full.size());
}

// ---------------------------------------------------- 9-10 coalesced reads

// 9
TEST(Parser, ConsumedAllowsSecondCommand) {
  const std::string first = "*1\r\n$4\r\nPING\r\n";
  const std::string second = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
  const std::string in = first + second;

  auto p1 = parseOnce(in);
  ASSERT_EQ(p1.status, ParseStatus::kOk);
  ASSERT_EQ(p1.argv.size(), 1u);
  EXPECT_EQ(p1.argv[0], "PING");
  EXPECT_EQ(p1.consumed, first.size());

  auto p2 = parseOnce(std::string_view(in).substr(p1.consumed));
  ASSERT_EQ(p2.status, ParseStatus::kOk);
  ASSERT_EQ(p2.argv.size(), 2u);
  EXPECT_EQ(p2.argv[0], "GET");
  EXPECT_EQ(p2.argv[1], "k");
  EXPECT_EQ(p2.consumed, second.size());
}

// 10
TEST(Parser, TrailingPartialAfterCompleteCommand) {
  const std::string first = "*1\r\n$4\r\nPING\r\n";
  const std::string in = first + "*2\r\n$3\r\nGE";

  auto p1 = parseOnce(in);
  ASSERT_EQ(p1.status, ParseStatus::kOk);
  EXPECT_EQ(p1.consumed, first.size());

  auto p2 = parseOnce(std::string_view(in).substr(p1.consumed));
  EXPECT_EQ(p2.status, ParseStatus::kNeedMore);
  EXPECT_EQ(p2.consumed, 0u);
}

// ------------------------------------------------------ 11-15 inline commands

// 11
TEST(Parser, ParsesInlinePing) {
  auto p = parseOnce("PING\r\n");
  EXPECT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 1u);
  EXPECT_EQ(p.argv[0], "PING");
  EXPECT_EQ(p.consumed, 6u);
}

// 12
TEST(Parser, ParsesInlineWithArguments) {
  auto p = parseOnce("SET foo bar\r\n");
  EXPECT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 3u);
  EXPECT_EQ(p.argv[0], "SET");
  EXPECT_EQ(p.argv[1], "foo");
  EXPECT_EQ(p.argv[2], "bar");
}

// 13 -- telnet sends this.
TEST(Parser, InlineToleratesBareLf) {
  auto p = parseOnce("PING\n");
  EXPECT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 1u);
  EXPECT_EQ(p.argv[0], "PING");
  EXPECT_EQ(p.consumed, 5u);
}

// 14
TEST(Parser, InlineCollapsesRepeatedSpaces) {
  auto p = parseOnce("SET   foo   bar\r\n");
  EXPECT_EQ(p.status, ParseStatus::kOk);
  ASSERT_EQ(p.argv.size(), 3u);
  EXPECT_EQ(p.argv[2], "bar");
}

// 15 -- a blank line is skipped, not answered and not an error.
TEST(Parser, EmptyInlineLineIsIgnored) {
  auto p = parseOnce("\r\n");
  EXPECT_EQ(p.status, ParseStatus::kOk);
  EXPECT_TRUE(p.argv.empty());
  EXPECT_EQ(p.consumed, 2u);
}

// ------------------------------------- 16-27 malformed and hostile input
//
// Every one of these must be kProtocolError (or, for 27, kNeedMore) rather
// than a crash, an over-read, or an allocation sized by the attacker.

// 16
TEST(Parser, NegativeMultibulkLengthIsError) {
  EXPECT_EQ(parseOnce("*-5\r\n").status, ParseStatus::kProtocolError);
}

// 17 -- $-1 is a reply-side null; it is not legal in a request.
TEST(Parser, NegativeBulkLengthIsError) {
  EXPECT_EQ(parseOnce("*1\r\n$-3\r\n").status, ParseStatus::kProtocolError);
  EXPECT_EQ(parseOnce("*1\r\n$-1\r\n").status, ParseStatus::kProtocolError);
}

// 18
TEST(Parser, MultibulkLengthOverLimitIsError) {
  EXPECT_EQ(parseOnce("*20000000\r\n").status, ParseStatus::kProtocolError);
}

// 19
TEST(Parser, BulkLengthOverLimitIsError) {
  EXPECT_EQ(parseOnce("*1\r\n$600000000\r\n").status, ParseStatus::kProtocolError);
}

// 20
TEST(Parser, NonNumericLengthIsError) {
  EXPECT_EQ(parseOnce("*abc\r\n").status, ParseStatus::kProtocolError);
}

// 21
TEST(Parser, LengthWithLeadingPlusIsError) {
  EXPECT_EQ(parseOnce("*+3\r\n").status, ParseStatus::kProtocolError);
}

// 22 -- request arrays contain bulk strings and nothing else.
TEST(Parser, ElementNotBulkStringIsError) {
  EXPECT_EQ(parseOnce("*1\r\n+OK\r\n").status, ParseStatus::kProtocolError);
}

// 23
TEST(Parser, DeclaredLengthShorterThanDataIsError) {
  EXPECT_EQ(parseOnce("*1\r\n$3\r\nfoobar\r\n").status, ParseStatus::kProtocolError);
}

// 24 -- enough bytes to see the terminator is wrong. Distinct from case 7.
TEST(Parser, BulkTerminatedByBareLfIsError) {
  EXPECT_EQ(parseOnce("*1\r\n$3\r\nfoo\n").status, ParseStatus::kProtocolError);
}

// 25
TEST(Parser, BulkTerminatedByGarbageIsError) {
  EXPECT_EQ(parseOnce("*1\r\n$3\r\nfooXX").status, ParseStatus::kProtocolError);
}

// 26
TEST(Parser, HeaderTerminatedByBareLfIsError) {
  EXPECT_EQ(parseOnce("*1\n").status, ParseStatus::kProtocolError);
}

// 27 -- 400MB is under the 512MB limit, so this is a legal declaration whose
// body has not arrived: kNeedMore. The point is that saying "400MB" must not
// make the parser reserve 400MB.
TEST(Parser, HugeButLegalLengthDoesNotPreallocate) {
  const long before = residentKb();
  ASSERT_GT(before, 0) << "could not read /proc/self/statm";

  auto p = parseOnce("*1\r\n$400000000\r\nabcd");
  EXPECT_EQ(p.status, ParseStatus::kNeedMore);
  EXPECT_EQ(p.consumed, 0u);

  const long after = residentKb();
  ASSERT_GT(after, 0);
  EXPECT_LT(after - before, 1024) << "parser grew RSS by " << (after - before) << "kB";
}

// Added in stage 8, after review. The bulk-length case above covers payload;
// this covers the argv metadata, which had the same flaw and was missed. A
// declared element count is attacker-controlled, and reserving against it
// allocates a million string_views -- sixteen megabytes per connection -- for
// a command whose first element has not arrived and may never arrive.
TEST(Parser, HugeElementCountDoesNotPreallocateArgv) {
  const long before = residentKb();
  ASSERT_GT(before, 0);

  std::vector<std::string_view> argv;
  std::size_t consumed = 0;
  for (int i = 0; i < 50; ++i) {
    argv.clear();
    argv.shrink_to_fit();
    EXPECT_EQ(parse("*1048576\r\n", argv, consumed), ParseStatus::kNeedMore);
    EXPECT_EQ(consumed, 0u);
    EXPECT_LT(argv.capacity(), 1024u)
        << "argv was reserved against the declared count";
  }

  const long after = residentKb();
  ASSERT_GT(after, 0);
  EXPECT_LT(after - before, 1024) << "grew RSS by " << (after - before) << "kB";
}
