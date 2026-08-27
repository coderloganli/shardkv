#include "store/shard.h"

#include <string>

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

// Deliberately does not sweep. An expired key that nobody has looked up since
// is still in the table and is still counted here, which is the honest answer
// while only lazy expiry exists. A test pins this so that adding sampled expiry
// cannot happen without noticing.
std::size_t Shard::size() const { return map_.size(); }

const Clock& Shard::clock() const { return *clock_; }

}  // namespace shardkv
