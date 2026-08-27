#include <charconv>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include "net/loop.h"
#include "store/clock.h"
#include "store/shard.h"

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
      "usage: shardkv [--port N] [--shards N]\n"
      "\n"
      "  --port N     TCP port to listen on (default 6380, deliberately not\n"
      "               6379 so a real redis-server can run alongside as the\n"
      "               control group for measurements)\n"
      "  --shards N   number of event loops, one per core (default 1). Only 1\n"
      "               is accepted today; the sharded loops are the next piece\n"
      "               of work. The option exists now so that scripts written\n"
      "               against it keep working when N > 1 arrives.\n",
      stderr);
}

}  // namespace

int main(int argc, char** argv) {
  int port = 6380;
  int shards = 1;

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
      const auto parsed = wholeNumber(argv[++i]);
      if (!parsed.has_value()) {
        std::fprintf(stderr, "shardkv: bad shard count '%s'\n", argv[i]);
        return 2;
      }
      shards = *parsed;
      if (shards != 1) {
        // Refused, not silently ignored. A benchmark run with --shards 8 that
        // quietly used one loop would produce a number labelled as something
        // it is not, which is the failure this project exists to argue
        // against.
        std::fprintf(stderr,
                     "shardkv: --shards %s is not supported yet; only 1 is. "
                     "Sharded loops are the next piece of work.\n",
                     argv[i]);
        return 2;
      }
      continue;
    }
    std::fprintf(stderr, "shardkv: unrecognised argument '%s'\n", arg.c_str());
    usage();
    return 2;
  }

  try {
    shardkv::SteadyClock clock;
    shardkv::Shard shard(clock);
    shardkv::Loop loop(static_cast<std::uint16_t>(port), shard);
    std::fprintf(stderr, "shardkv listening on port %u, %d shard\n",
                 static_cast<unsigned>(loop.port()), shards);
    loop.run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "shardkv: %s\n", e.what());
    return 1;
  }
  return 0;
}
