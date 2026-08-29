#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#include "store/hash.h"
#include "store/clock.h"
#include "store/value.h"

namespace shardkv {

// What one sampling pass did. Both numbers are reported because a pass that
// said nothing would be indistinguishable from one that never ran, and because
// the caller decides whether to run another from the ratio between them.
struct SampleResult {
  std::size_t visited = 0;
  std::size_t erased = 0;
};

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

  // The raw table size. This deliberately does NOT sweep for expired keys, and
  // still does not: DBSIZE reports the table as it stands. What changed when
  // sampled expiry landed is not this function but the table, which now
  // converges because sampleExpired() reaps it.
  std::size_t size() const;

  // One bounded pass of the background sampler: visit at most `limit` keys from
  // where the last pass stopped, erase the ones past their deadline, and report
  // what it did. The lazy half of expiry cannot reach a key nobody looks up
  // again; this is what does.
  //
  // Called by the owning loop's timer, on the owning loop's thread, like
  // everything else that touches this shard.
  SampleResult sampleExpired(std::size_t limit);

  const Clock& clock() const;

  // The number of entries in the fullest bucket.
  //
  // For one test, which needs to know that the table actually contains a bucket
  // longer than a sampling pass's limit -- otherwise the property that test is
  // named for is not present in the table and it would pass having asserted
  // nothing. The bucket layout is the container's business and no production
  // code looks at this; ReplySlots::pendingForTest() exists for the same kind
  // of reason.
  std::size_t largestBucketForTest() const;

 private:
  const Clock* clock_ = nullptr;
  // Where the next sampling pass resumes. A bucket index alone would not do:
  // a bucket holding more entries than one pass's limit would be restarted from
  // its front every time, and everything past the limit would never be reached.
  std::size_t sample_bucket_ = 0;
  std::size_t sample_offset_ = 0;
  // KeyHash rather than std::hash -- see store/hash.h. is_transparent lets a
  // string_view look up without materialising a std::string first.
  std::unordered_map<std::string, Value, KeyHash, std::equal_to<>> map_;
};

}  // namespace shardkv
