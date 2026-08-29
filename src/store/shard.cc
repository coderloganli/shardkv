#include "store/shard.h"

#include <algorithm>
#include <string>
#include <vector>

namespace shardkv {

Shard::Shard(const Clock& clock) : clock_(&clock) {}

// The single place expiry is enforced. Every command reaches a value through
// here, so no command can observe an expired key -- which is why the rule lives
// in one function rather than being repeated in twenty command handlers.
Value* Shard::lookup(std::string_view key) {
  const auto it = map_.find(key);
  if (it == map_.end()) return nullptr;

  if (it->second.expires_at.has_value() &&
      clock_->now() >= *it->second.expires_at) {
    map_.erase(it);  // erased, not merely hidden
    return nullptr;
  }
  return &it->second;
}

void Shard::set(std::string_view key, std::string_view data) {
  auto& value = map_[std::string(key)];
  value.data.assign(data);
  value.expires_at.reset();  // SET without EX clears any existing TTL
}

bool Shard::erase(std::string_view key) {
  const auto it = map_.find(key);
  if (it == map_.end()) return false;
  map_.erase(it);
  return true;
}

void Shard::clear() { map_.clear(); }

// Deliberately does not sweep, and still does not. DBSIZE answers for the table
// as it stands; sampleExpired() is what makes that answer converge. Sweeping
// here instead would make a read command's cost depend on how much rubbish had
// accumulated, which is the thing the sampler exists to avoid.
std::size_t Shard::size() const { return map_.size(); }

// One bounded pass of the background sampler.
//
// The lazy half of expiry cannot reach a key nobody looks up again. This walks
// the table on a cursor instead, so a lap with no intervening rehash visits
// every key and none can hide behind a bucket the sampler keeps missing. A
// rehash moves elements between buckets and can make one lap visit a key twice
// or not at all; the sweep carries on and the next lap covers the table again,
// which is the honest guarantee rather than a per-lap one the container does
// not offer.
SampleResult Shard::sampleExpired(std::size_t limit) {
  SampleResult result;
  if (limit == 0 || map_.empty()) return result;

  const std::size_t buckets = map_.bucket_count();
  if (buckets == 0) return result;
  if (sample_bucket_ >= buckets) {  // the table rehashed or was cleared
    sample_bucket_ = 0;
    sample_offset_ = 0;
  }

  const TimePoint now = clock_->now();

  // Collected, not erased in place: a bucket walk yields local_iterators, which
  // unordered_map::erase will not take, and erasing mid-walk would invalidate
  // the walk in any case. Erasing an element invalidates references and
  // pointers to THAT element alone, so these stay good as the list is worked
  // through.
  std::vector<const std::string*> doomed;

  bool stopped_mid_bucket = false;
  std::size_t doomed_before_this_bucket = 0;

  for (std::size_t lap = 0; lap < buckets && result.visited < limit; ++lap) {
    doomed_before_this_bucket = doomed.size();

    auto it = map_.begin(sample_bucket_);
    const auto end = map_.end(sample_bucket_);
    for (std::size_t skipped = 0; skipped < sample_offset_ && it != end; ++skipped) {
      ++it;
    }

    stopped_mid_bucket = false;
    while (it != end) {
      if (result.visited == limit) {
        stopped_mid_bucket = true;
        break;
      }
      ++result.visited;
      ++sample_offset_;
      if (it->second.expires_at.has_value() && now >= *it->second.expires_at) {
        doomed.push_back(&it->first);
      }
      ++it;
    }

    if (stopped_mid_bucket) break;
    sample_bucket_ = (sample_bucket_ + 1) % buckets;
    sample_offset_ = 0;
  }

  // Erasing from the bucket the cursor is resting in shifts the entries behind
  // it forward, so the offset has to come back by however many went. Where the
  // pass finished a bucket and moved on, the offset is already zero and there
  // is nothing to correct.
  if (stopped_mid_bucket) {
    const std::size_t erased_here = doomed.size() - doomed_before_this_bucket;
    sample_offset_ = erased_here >= sample_offset_ ? 0 : sample_offset_ - erased_here;
  }

  for (const std::string* key : doomed) {
    const auto it = map_.find(*key);
    if (it != map_.end()) map_.erase(it);
  }
  result.erased = doomed.size();
  return result;
}

std::size_t Shard::largestBucketForTest() const {
  std::size_t largest = 0;
  for (std::size_t bucket = 0; bucket < map_.bucket_count(); ++bucket) {
    largest = std::max(largest, map_.bucket_size(bucket));
  }
  return largest;
}

const Clock& Shard::clock() const { return *clock_; }

}  // namespace shardkv
