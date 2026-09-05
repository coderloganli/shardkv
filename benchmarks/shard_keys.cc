// Prints keys belonging to a chosen shard, one per line.
//
// The benchmark scripts need this because `redis-benchmark` generates
// `key:__rand_int__` and cannot be told to aim at a shard, so a load that is
// entirely local or entirely remote cannot be built out of it alone.
//
// It reuses the server's own `shardForKey` rather than reimplementing the
// mapping. A benchmark that disagreed with the server about which shard owns a
// key would not fail -- it would produce a confident wrong number, which is
// worse.
//
//   shard_keys --shards N --shard I --count K [--prefix P]

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "store/hash.h"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: shard_keys --shards N --shard I --count K [--prefix P]\n"
               "\n"
               "  Prints K distinct keys that belong to shard I when the\n"
               "  keyspace is split across N shards, using the same hash the\n"
               "  server uses.\n"
               "\n"
               "  --prefix defaults to \"key:\". I must be less than N.\n");
}

std::optional<std::size_t> wholeNumber(const char* text) {
  errno = 0;
  char* end = nullptr;
  const long long value = std::strtoll(text, &end, 10);
  // errno, not just the return value: strtoll saturates at LLONG_MAX on
  // overflow, so "99999999999999999999" would otherwise be accepted as a very
  // large but plausible count.
  if (end == text || *end != '\0' || value < 0 || errno == ERANGE) return std::nullopt;
  return static_cast<std::size_t>(value);
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::size_t> shards;
  std::optional<std::size_t> shard;
  std::optional<std::size_t> count;
  std::string prefix = "key:";

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;

    if (arg == "--shards" && has_value) {
      shards = wholeNumber(argv[++i]);
    } else if (arg == "--shard" && has_value) {
      shard = wholeNumber(argv[++i]);
    } else if (arg == "--count" && has_value) {
      count = wholeNumber(argv[++i]);
    } else if (arg == "--prefix" && has_value) {
      prefix = argv[++i];
    } else {
      usage();
      return 2;
    }
  }

  if (!shards || !shard || !count) {
    usage();
    return 2;
  }

  // Refusing rather than printing nothing, and this is the whole reason the
  // check is here: a script reads an empty stdout with a zero exit status as a
  // valid but empty key set, and would benchmark against nothing and report it.
  if (*shards == 0) {
    std::fprintf(stderr, "shard_keys: --shards must be at least 1\n");
    return 1;
  }
  if (*shard >= *shards) {
    std::fprintf(stderr, "shard_keys: shard %zu does not exist with %zu shard(s)\n",
                 *shard, *shards);
    return 1;
  }

  // Candidates in order, keeping the ones that land on the wanted shard. With a
  // hash that avalanches, about one in `shards` qualifies, so the search is
  // linear in the number of keys asked for.
  //
  // The bound is not decoration. A caller can ask for more keys than the search
  // will ever produce -- a count larger than the candidates tried, or a
  // pathological prefix -- and without it the tool would sit in this loop
  // forever while whatever ran it waited. Failing is the better answer, and it
  // says how far it got.
  const std::size_t kMaxCandidates = 1000 * *shards * (*count + 1) + 100000;

  std::size_t found = 0;
  std::size_t tried = 0;
  for (; found < *count && tried < kMaxCandidates; ++tried) {
    const std::string candidate = prefix + std::to_string(tried);
    if (shardkv::shardForKey(candidate, *shards) != *shard) continue;
    std::printf("%s\n", candidate.c_str());
    ++found;
  }

  if (found < *count) {
    std::fprintf(stderr,
                 "shard_keys: only found %zu of %zu keys for shard %zu of %zu "
                 "after %zu candidates\n",
                 found, *count, *shard, *shards, tried);
    return 1;
  }

  return 0;
}
