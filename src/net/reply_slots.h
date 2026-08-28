#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "base/buffer.h"

// InfoField, the layout of a kInfo aggregate's parts.
#include "commands/info_fields.h"

namespace shardkv {

// What a multi-key command is still waiting for.
enum class AggregateKind {
  kArray,   // MGET: parts, in argument order
  kCount,   // DEL, EXISTS, DBSIZE: a sum
  kStatus,  // MSET, FLUSHDB: +OK once every group is in
  kInfo,    // INFO: one key count per shard, spliced into a prepared body
};

struct Aggregate {
  AggregateKind kind = AggregateKind::kArray;
  std::uint32_t remaining = 0;
  // Indexed by the argument's ORIGINAL position, so the reply reads in the
  // order the client wrote it however the shards were scheduled. Owned bytes,
  // never views.
  std::vector<std::optional<std::string>> parts;
  std::int64_t accumulator = 0;
  // kInfo only: everything in the INFO body that does not depend on a shard.
  // Kept here so this header need know nothing about the server's counters.
  std::string header;
};

// A slot is empty (waiting), ready (encoded RESP), or mid-aggregate.
using Slot = std::variant<std::monostate, std::string, Aggregate>;

// The ordered reply queue for one connection.
//
// RESP has no request identifiers, so replies must leave in command order
// whatever order they become ready in. See
// docs/adr/0006-replies-are-written-into-ordered-slots.md
//
// Touched only by the connection's own thread, start to finish.
//
class ReplySlots {
 public:
  // Called when a command is parsed, before it is known whether the work is
  // local.
  std::uint32_t reserve();

  void fill(std::uint32_t slot, std::string resp);

  void beginAggregate(std::uint32_t slot, Aggregate aggregate);

  // One group's answer. When the last one lands the aggregate is encoded and
  // the slot becomes ready.
  void contribute(std::uint32_t slot, std::uint32_t index, std::optional<std::string> part);
  void contributeCount(std::uint32_t slot, std::int64_t count);

  // Appends the longest run of ready slots from the front and drops them. A gap
  // stops it; everything behind the gap waits.
  void takeReadyPrefix(Buffer& out);

  bool idle() const;

  // Outstanding slots, for tests that would otherwise have to assert something
  // unfalsifiable about memory.
  std::size_t pendingForTest() const;

 private:
  // Slot numbers rise for the life of the connection while the deque is popped
  // from the front, so the two drift apart; base_ is the offset. Returns
  // nullptr for a slot already flushed, which is how a late reply for a
  // finished command is dropped rather than corrupting a live one.
  Slot* at(std::uint32_t slot);

  // Encodes and replaces the slot once the last group has contributed.
  void finishIfComplete(Slot& slot);

  std::deque<Slot> slots_;
  std::uint32_t next_ = 0;  // next number to hand out
  std::uint32_t base_ = 0;  // the number slots_[0] carries
};

}  // namespace shardkv
