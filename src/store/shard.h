#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#include "store/hash.h"
#include "store/clock.h"
#include "store/value.h"

namespace shardkv {

// One slice of the keyspace: a hash table, and the only thread allowed to touch
// it is the one that owns it. Nothing here is thread-safe, and nothing here
// needs to be -- that is the whole point of the architecture.
//
// With one loop there is one shard. Later there are N, one per core.
//
// The table is std::unordered_map for v1
// (docs/adr/0001-use-the-standard-library-hash-table-first.md): a hand-written
// open-addressing table is very likely faster, but writing it before there is a
// baseline turns a measurable result into an assertion.
class Shard {
 public:
  explicit Shard(const Clock& clock);

  // The one place expiry is enforced. A key past its deadline is erased here
  // and reported as missing, so no command can ever observe an expired value.
  //
  // Returns nullptr when the key is absent or expired. The pointer is
  // invalidated by the next mutation of this shard.
  Value* lookup(std::string_view key);

  void set(std::string_view key, std::string_view data);
  bool erase(std::string_view key);
  void clear();

  // The raw table size. This deliberately does NOT sweep for expired keys, so
  // it can count keys that are already dead but have not been looked up since.
  // That gap closes when sampled expiry lands; until then it is the honest
  // answer, and a test pins it so the change is noticed.
  std::size_t size() const;

  const Clock& clock() const;

 private:
  const Clock* clock_ = nullptr;
  // KeyHash rather than std::hash -- see store/hash.h. is_transparent lets a
  // string_view look up without materialising a std::string first.
  std::unordered_map<std::string, Value, KeyHash, std::equal_to<>> map_;
};

}  // namespace shardkv
