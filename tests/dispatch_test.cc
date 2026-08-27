// Test cases 81-90 from task.md.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "commands/dispatch.h"
#include "helpers.h"

using namespace shardkv;
using shardkv::testing::Fixture;
using shardkv::testing::run;
using shardkv::testing::runFor;

// 81 -- and the connection survives it.
TEST(Dispatch, UnknownCommandReturnsError) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "NOSUCHCMD a b"), "-ERR unknown command 'NOSUCHCMD'\r\n");
  EXPECT_EQ(run(f.shard, "PING"), "+PONG\r\n");
}

// 82 -- this is the protocol's documented downgrade path, not just tidiness: a
// client discovers the server speaks RESP2 by getting this error back.
TEST(Dispatch, HelloIsRefusedAsUnknownCommand) {
  Fixture f;
  std::string out;
  const auto after = runFor(f.shard, {"HELLO", "3"}, out);
  EXPECT_EQ(out, "-ERR unknown command 'HELLO'\r\n");
  EXPECT_EQ(after, AfterCommand::kKeepOpen);
}

// 83
TEST(Dispatch, CommandReturnsEmptyArray) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "COMMAND"), "*0\r\n");
}

// 84
TEST(Dispatch, CommandDocsReturnsEmptyArray) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "COMMAND DOCS"), "*0\r\n");
}

// 85
TEST(Dispatch, CommandNameIsCaseInsensitive) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "set k v"), "+OK\r\n");
  EXPECT_EQ(run(f.shard, "SET k v"), "+OK\r\n");
  EXPECT_EQ(run(f.shard, "SeT k v"), "+OK\r\n");
}

// 86
TEST(Dispatch, WrongArityReturnsError) {
  Fixture f;
  const std::string expected = "-ERR wrong number of arguments for 'get' command\r\n";
  EXPECT_EQ(run(f.shard, "GET"), expected);
  EXPECT_EQ(run(f.shard, "GET a b"), expected);
}

// 87
TEST(Dispatch, PingReturnsPong) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "PING"), "+PONG\r\n");
}

// 88
TEST(Dispatch, PingWithArgumentEchoesIt) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "PING hello"), "$5\r\nhello\r\n");
}

// 89
TEST(Dispatch, EchoReturnsArgument) {
  Fixture f;
  EXPECT_EQ(run(f.shard, "ECHO hi"), "$2\r\nhi\r\n");
}

// 90 -- the reply is written first, then the connection goes.
TEST(Dispatch, QuitRepliesOkThenCloses) {
  Fixture f;
  std::string out;
  const auto after = runFor(f.shard, {"QUIT"}, out);
  EXPECT_EQ(out, "+OK\r\n");
  EXPECT_EQ(after, AfterCommand::kClose);
}

// Added in stage 8, after review. A command name is whatever bytes the client
// sent, and a bulk string may hold any of them. Echoing it raw into an error
// reply is response-line injection: the request below is entirely legal RESP,
// and a raw echo would end the error line early and let the sender dictate the
// bytes a client reads as its next reply.
TEST(Dispatch, UnknownCommandNameCannotInjectAReplyLine) {
  Fixture f;
  std::string out;
  runFor(f.shard, {"BAD\r\n+INJECTED"}, out);

  // Exactly one reply line, and the CRLF that ends it is the only one.
  EXPECT_EQ(out.find("\r\n"), out.size() - 2) << out;
  EXPECT_EQ(out.find("+INJECTED\r\n"), std::string::npos) << out;
  // The CR and LF come back as their escaped spellings, so the name is still
  // legible in a log without being able to end the line.
  EXPECT_EQ(out, "-ERR unknown command 'BAD\\x0d\\x0a+INJECTED'\r\n");
}

TEST(Dispatch, WrongArityNameCannotInjectAReplyLine) {
  Fixture f;
  std::string out;
  // GET is a known command; the injection attempt rides on a name that reaches
  // the arity check instead.
  runFor(f.shard, {"GET\r\n+INJECTED", "a"}, out);
  EXPECT_EQ(out.find("\r\n"), out.size() - 2) << out;
  EXPECT_EQ(out.find("+INJECTED\r\n"), std::string::npos) << out;
}

TEST(Dispatch, EchoedCommandNameIsCapped) {
  Fixture f;
  std::string out;
  runFor(f.shard, {std::string(4096, 'Z')}, out);
  EXPECT_LT(out.size(), 256u) << "an error reply should not echo a whole payload";
  EXPECT_NE(out.find("..."), std::string::npos) << out;
}
