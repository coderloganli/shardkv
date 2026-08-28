#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/buffer.h"
#include "commands/dispatch.h"
#include "commands/router.h"
#include "net/reply_slots.h"
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

// One shard, and every key is local. This is what keeps the command tests from
// this project's first task working unchanged: they were written against a
// synchronous dispatch, and with a single shard nothing is ever sent anywhere,
// so their bodies still read the same.
class LocalOnlyRouter final : public ShardRouter {
 public:
  explicit LocalOnlyRouter(Shard& shard) : shard_(&shard) {}

  std::size_t shardCount() const override { return 1; }
  std::size_t localShard() const override { return 0; }
  Shard& local() override { return *shard_; }

  void send(std::size_t, CrossShardRequest) override {
    // Unreachable with one shard: every key maps to shard 0. If this ever
    // fires, the routing arithmetic is wrong, and silence would hide it.
    throw std::logic_error("LocalOnlyRouter: a single-shard server sent a message");
  }

 private:
  Shard* shard_ = nullptr;
};

// A shard wired to a clock the test moves by hand. Sleeping would make the
// expiry tests slow when they pass and flaky when they do not.
struct Fixture {
  ManualClock clock;
  Shard shard{clock};
  LocalOnlyRouter router{shard};
  ReplySlots slots;
};

namespace detail {

inline std::string dispatchAndTake(Fixture& f, const std::vector<std::string>& parts,
                                   AfterCommand* after_out) {
  std::vector<std::string_view> argv;
  argv.reserve(parts.size());
  for (const auto& part : parts) argv.emplace_back(part);

  const std::uint32_t slot = f.slots.reserve();
  const AfterCommand after =
      dispatch(f.router, f.slots, slot, /*conn_id=*/1, argv, kNoConnections);
  if (after_out != nullptr) *after_out = after;

  Buffer out;
  f.slots.takeReadyPrefix(out);
  return std::string(out.readable());
}

}  // namespace detail

// Splits a command written the way a person would write it -- `SET k v` -- and
// dispatches it, returning the raw RESP reply. Test cases in task.md are
// written in this form, so this keeps the test and the case looking alike.
//
// The splitting is whitespace-only and deliberately dumb: a value containing a
// space is built with runArgv instead.
inline std::string run(Fixture& f, std::string_view command_line) {
  std::vector<std::string> parts;
  std::size_t i = 0;
  while (i < command_line.size()) {
    while (i < command_line.size() && command_line[i] == ' ') ++i;
    const std::size_t start = i;
    while (i < command_line.size() && command_line[i] != ' ') ++i;
    if (i > start) parts.emplace_back(command_line.substr(start, i - start));
  }
  return detail::dispatchAndTake(f, parts, nullptr);
}

inline std::string runArgv(Fixture& f, const std::vector<std::string>& parts) {
  return detail::dispatchAndTake(f, parts, nullptr);
}

inline AfterCommand runFor(Fixture& f, const std::vector<std::string>& parts,
                           std::string& out) {
  AfterCommand after = AfterCommand::kKeepOpen;
  out = detail::dispatchAndTake(f, parts, &after);
  return after;
}

// The command tests written for the single-threaded server call these as
// run(f.shard, ...), because a shard was all there was to talk to. These
// overloads keep every one of those bodies exactly as it was: with one shard
// there is nothing to route and nothing to wait for, so a throwaway router and
// slot queue per call is all the adaptation needed.
namespace detail {
struct ScratchFixture {
  explicit ScratchFixture(Shard& s) : shard(&s), router(s) {}
  Shard* shard;
  LocalOnlyRouter router;
  ReplySlots slots;
};

inline std::string dispatchOnShard(Shard& shard, const std::vector<std::string>& parts,
                                   AfterCommand* after_out) {
  ScratchFixture scratch(shard);
  std::vector<std::string_view> argv;
  argv.reserve(parts.size());
  for (const auto& part : parts) argv.emplace_back(part);

  const std::uint32_t slot = scratch.slots.reserve();
  const AfterCommand after =
      dispatch(scratch.router, scratch.slots, slot, /*conn_id=*/1, argv, kNoConnections);
  if (after_out != nullptr) *after_out = after;

  Buffer out;
  scratch.slots.takeReadyPrefix(out);
  return std::string(out.readable());
}

inline std::vector<std::string> split(std::string_view line) {
  std::vector<std::string> parts;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ') ++i;
    if (i > start) parts.emplace_back(line.substr(start, i - start));
  }
  return parts;
}
}  // namespace detail

inline std::string run(Shard& shard, std::string_view command_line) {
  return detail::dispatchOnShard(shard, detail::split(command_line), nullptr);
}

inline std::string runArgv(Shard& shard, const std::vector<std::string>& parts) {
  return detail::dispatchOnShard(shard, parts, nullptr);
}

inline AfterCommand runFor(Shard& shard, const std::vector<std::string>& parts,
                           std::string& out) {
  AfterCommand after = AfterCommand::kKeepOpen;
  out = detail::dispatchOnShard(shard, parts, &after);
  return after;
}

}  // namespace shardkv::testing
