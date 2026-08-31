// Test cases 1-5 from task.md.
//
// The key-picking tool, exercised as a tool: every case here runs the built
// `shard_keys` binary and reads its standard output and its exit status. None of
// them calls shardForKey to produce the keys -- only to check them. A case that
// recomputed the mapping in the test would pass whether or not the executable
// worked, and the executable is the deliverable: it is what the benchmark
// scripts will call.
//
// Cases 6-9, which need a running server, live in sharding_test.cc beside the
// multi-loop fixture they require.

#include <array>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "store/hash.h"

using namespace shardkv;

namespace {

#ifndef SHARD_KEYS_PATH
#error "SHARD_KEYS_PATH must name the built shard_keys binary; see CMakeLists.txt"
#endif

struct ToolRun {
  int status = -1;
  std::vector<std::string> lines;
};

// Runs shard_keys with the given arguments, capturing stdout and the exit
// status. Standard error is left alone so a failing case still shows the tool's
// own message in the test log.
ToolRun runTool(const std::string& args) {
  ToolRun out;
  const std::string command = std::string(SHARD_KEYS_PATH) + " " + args;

  std::FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    ADD_FAILURE() << "could not run: " << command;
    return out;
  }

  std::array<char, 512> buffer{};
  std::string line;
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    line.assign(buffer.data());
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (!line.empty()) out.lines.push_back(line);
  }

  const int raw = ::pclose(pipe);
  out.status = (raw == -1) ? -1 : (raw >> 8);
  return out;
}

}  // namespace

// 1 -- the whole point of the tool. Checked with the same shardForKey the server
// uses, because a tool that agreed with a different mapping would send the
// benchmark at the wrong shard and report a confident wrong number.
TEST(ShardKeys, EveryKeyBelongsToTheShardItWasAskedFor) {
  for (const std::size_t shards : {1u, 2u, 4u, 8u}) {
    for (std::size_t shard = 0; shard < shards; ++shard) {
      const ToolRun run = runTool("--shards " + std::to_string(shards) + " --shard " +
                              std::to_string(shard) + " --count 50");

      ASSERT_EQ(run.status, 0) << "shards=" << shards << " shard=" << shard;
      ASSERT_EQ(run.lines.size(), 50u) << "shards=" << shards << " shard=" << shard;

      for (const std::string& key : run.lines) {
        EXPECT_EQ(shardForKey(key, shards), shard)
            << "key '" << key << "' does not belong to shard " << shard << " of "
            << shards;
      }
    }
  }
}

// 2 -- a tool that printed one key fifty times would satisfy case 1 exactly and
// be useless: the benchmark would hit a single key and measure the cache.
TEST(ShardKeys, TheKeysAreDistinct) {
  const ToolRun run = runTool("--shards 4 --shard 1 --count 200");
  ASSERT_EQ(run.status, 0);
  ASSERT_EQ(run.lines.size(), 200u);

  const std::set<std::string> unique(run.lines.begin(), run.lines.end());
  EXPECT_EQ(unique.size(), run.lines.size()) << "the tool repeated itself";
}

// 3 -- the failure that would be silent. Exiting 0 with no output reads to a
// shell script as a valid but empty key set, and the benchmark would run against
// nothing and report it.
TEST(ShardKeys, ItRefusesAShardIndexItCannotSatisfy) {
  const ToolRun run = runTool("--shards 4 --shard 4 --count 10");
  EXPECT_NE(run.status, 0) << "an out-of-range shard was accepted";
  EXPECT_TRUE(run.lines.empty()) << "it printed keys for a shard that does not exist";
}

// 4
TEST(ShardKeys, ItRefusesZeroShards) {
  const ToolRun run = runTool("--shards 0 --shard 0 --count 10");
  EXPECT_NE(run.status, 0) << "zero shards was accepted; shardForKey would divide by it";
  EXPECT_TRUE(run.lines.empty());
}

// 5 -- with one shard everything is local by construction, which is the case the
// cross-shard script uses as its control.
TEST(ShardKeys, ThePrefixIsHonouredAndOneShardOwnsEverything) {
  const ToolRun run = runTool("--shards 1 --shard 0 --count 20 --prefix bench:");
  ASSERT_EQ(run.status, 0);
  ASSERT_EQ(run.lines.size(), 20u);

  for (const std::string& key : run.lines) {
    EXPECT_EQ(key.rfind("bench:", 0), 0u) << "prefix ignored: " << key;
    EXPECT_EQ(shardForKey(key, 1), 0u);
  }
}
