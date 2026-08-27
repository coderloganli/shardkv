#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "commands/dispatch.h"
#include "proto/parser.h"
#include "store/clock.h"
#include "store/shard.h"

namespace shardkv::testing {

// The unit tests drive dispatch directly, with no loop behind it. The live
// connection count is asserted over TCP instead, in the integration tests.
inline constexpr LoopStats kNoConnections{};

// What one parse() call said. A small struct so a test can read like the case
// it came from.
struct Parsed {
  ParseStatus status;
  std::vector<std::string_view> argv;
  std::size_t consumed = 0;
};

inline Parsed parseOnce(std::string_view in) {
  Parsed p;
  p.status = parse(in, p.argv, p.consumed);
  return p;
}

// Splits a command written the way a person would write it -- `SET k v` -- and
// dispatches it, returning the raw RESP reply. Test cases in task.md are
// written in this form, so this keeps the test and the case looking alike.
//
// The splitting is whitespace-only and deliberately dumb: a value containing a
// space is built with runArgv instead.
inline std::string run(Shard& shard, std::string_view command_line) {
  std::vector<std::string> parts;
  std::size_t i = 0;
  while (i < command_line.size()) {
    while (i < command_line.size() && command_line[i] == ' ') ++i;
    std::size_t start = i;
    while (i < command_line.size() && command_line[i] != ' ') ++i;
    if (i > start) parts.emplace_back(command_line.substr(start, i - start));
  }
  std::vector<std::string_view> argv;
  argv.reserve(parts.size());
  for (const auto& part : parts) argv.emplace_back(part);

  std::string out;
  dispatch(shard, argv, out, kNoConnections);
  return out;
}

inline std::string runArgv(Shard& shard, const std::vector<std::string>& parts) {
  std::vector<std::string_view> argv;
  argv.reserve(parts.size());
  for (const auto& part : parts) argv.emplace_back(part);
  std::string out;
  dispatch(shard, argv, out, kNoConnections);
  return out;
}

inline AfterCommand runFor(Shard& shard,
                           const std::vector<std::string>& parts,
                           std::string& out) {
  std::vector<std::string_view> argv;
  argv.reserve(parts.size());
  for (const auto& part : parts) argv.emplace_back(part);
  return dispatch(shard, argv, out, kNoConnections);
}

// A shard wired to a clock the test moves by hand. Sleeping would make the
// expiry tests slow when they pass and flaky when they do not.
struct Fixture {
  ManualClock clock;
  Shard shard{clock};
};

}  // namespace shardkv::testing
