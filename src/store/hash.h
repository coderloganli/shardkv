#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

namespace shardkv {

// The hash used both for the table inside a shard and for deciding which shard
// owns a key at all. XXH3, vendored as one header
// (docs/adr/0002-hash-keys-with-xxhash-not-std-hash.md).
//
// The second use is why this is not std::hash. `shard = hash(key) % N` decides
// which thread owns a key, so a hash that does not avalanche piles structured
// key names -- `user:1`, `user:2` -- onto a subset of shards, and the
// multi-core throughput claim quietly stops being true. std::hash promises no
// avalanche and differs between standard library implementations, which would
// make that a bug reproducing on one machine and not another.
inline std::uint64_t hashKey(std::string_view key) {
  return XXH3_64bits(key.data(), key.size());
}

// Which shard owns this key. With one loop the answer is always 0; the function
// exists now so that the next task adds loops rather than a concept.
inline std::size_t shardForKey(std::string_view key, std::size_t shard_count) {
  return static_cast<std::size_t>(hashKey(key) % shard_count);
}

// Drop-in hasher for the shard's unordered_map.
struct KeyHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view key) const {
    return static_cast<std::size_t>(hashKey(key));
  }
};

}  // namespace shardkv
