#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "net/server.h"
#include "store/clock.h"

namespace {

// A whole number and nothing else. atoi() reads "1x" as 1, so a benchmark
// launched with a typo would quietly run something other than what its label
// says -- which is the failure this project exists to argue against.
std::optional<int> wholeNumber(std::string_view text) {
  int value = 0;
  const auto* begin = text.data();
  const auto* end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
  return value;
}

void usage() {
  std::fputs(
      "usage: shardkv [--port N] [--shards N] [--pin]\n"
      "\n"
      "  --port N     TCP port to listen on (default 6380, deliberately not\n"
      "               6379 so a real redis-server can run alongside as the\n"
      "               control group for measurements)\n"
      "  --shards N   event loops, one thread and one slice of the keyspace\n"
      "               each. Defaults to the core count.\n"
      "  --pin        pin each loop to a core. Off by default: on a container\n"
      "               or a shared machine it can do more harm than good, and\n"
      "               where it earns its keep is a measurement run.\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  int port = 6380;
  // One loop per core unless told otherwise. The rule, including what to do
  // when the core count is unknowable, lives in server.h so a test can call it
  // rather than restate it.
  int shards = static_cast<int>(shardkv::defaultShardCount());
  bool pin = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--port" && i + 1 < argc) {
      const auto parsed = wholeNumber(argv[++i]);
      if (!parsed.has_value() || *parsed <= 0 || *parsed > 65535) {
        std::fprintf(stderr, "shardkv: bad port '%s'\n", argv[i]);
        return 2;
      }
      port = *parsed;
      continue;
    }
    if (arg == "--shards" && i + 1 < argc) {
      const auto parsed = shardkv::parseShardCountForTest(argv[++i]);
      if (!parsed.has_value()) {
        std::fprintf(stderr, "shardkv: bad shard count '%s'\n", argv[i]);
        return 2;
      }
      shards = static_cast<int>(*parsed);
      continue;
    }
    if (arg == "--pin") {
      pin = true;
      continue;
    }
    std::fprintf(stderr, "shardkv: unrecognised argument '%s'\n", arg.c_str());
    usage();
    return 2;
  }

  try {
    shardkv::SteadyClock clock;
    shardkv::Server::Options options;
    options.port = static_cast<std::uint16_t>(port);
    options.shards = static_cast<std::size_t>(shards);
    options.pin = pin;

    shardkv::Server server(options, clock);
    // "requested", not "pinned": affinity is applied by each loop as it starts,
    // and it can legitimately fail. INFO reports what actually happened.
    std::fprintf(stderr, "shardkv listening on port %u, %zu shards, pin %s\n",
                 static_cast<unsigned>(server.port()), server.shardCount(),
                 pin ? "requested" : "off");
    server.start();

    // The loops are the server; this thread has nothing left to do but stay out
    // of their way.
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "shardkv: %s\n", e.what());
    return 1;
  }
  return 0;
}
