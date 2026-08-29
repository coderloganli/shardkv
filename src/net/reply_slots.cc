#include "net/reply_slots.h"

#include <cassert>
#include <string>
#include <utility>

#include "proto/encoder.h"

namespace shardkv {
namespace {

// Turns a finished aggregate into the RESP its command owes the client.
std::string encodeAggregate(const Aggregate& aggregate) {
  std::string out;
  switch (aggregate.kind) {
    case AggregateKind::kArray:
      // By original argument position, not by the order the groups came back
      // in. This is the whole point of keeping `parts` indexed rather than
      // appended to.
      resp::encodeArray(aggregate.parts, out);
      break;
    case AggregateKind::kCount:
      resp::encodeInteger(aggregate.accumulator, out);
      break;
    case AggregateKind::kStatus:
      resp::encodeSimpleString("OK", out);
      break;
    case AggregateKind::kInfo: {
      // The parts are per-shard key counts, and the header is everything that
      // does not depend on a shard. Splicing here keeps this file ignorant of
      // what the server counts.
      // The parts are flat: field * loops + loop. Every one of them belongs to
      // a particular loop, and is labelled with that loop -- a value taken from
      // one loop and printed under another's name is not a smaller mistake than
      // a wrong number.
      const std::size_t fields = static_cast<std::size_t>(InfoField::kCount);
      const std::size_t loops = aggregate.parts.size() / fields;
      const auto part = [&](InfoField field, std::size_t loop) {
        return aggregate.parts[static_cast<std::size_t>(field) * loops + loop]
            .value_or("0");
      };

      std::string body = aggregate.header;

      // Pinned is all or nothing across the server: a client asking whether it
      // is measuring a pinned server wants one answer, and "some of the loops"
      // is not one.
      bool every_loop_pinned = loops > 0;
      std::int64_t cross_shard_total = 0;
      for (std::size_t i = 0; i < loops; ++i) {
        if (part(InfoField::kPinned, i) != "1") every_loop_pinned = false;
        cross_shard_total += std::stoll(part(InfoField::kCrossShardRequests, i));
      }
      body += "pinned:" + std::string(every_loop_pinned ? "1" : "0") + "\r\n";
      body += "cross_shard_requests:" + std::to_string(cross_shard_total) + "\r\n";

      body += "\r\n# Clients\r\n";
      for (std::size_t i = 0; i < loops; ++i) {
        const std::string n = std::to_string(i);
        body += "loop" + n + "_connections:" + part(InfoField::kConnections, i) + "\r\n";
        body += "loop" + n + "_short_writes:" + part(InfoField::kShortWrites, i) + "\r\n";
        body += "loop" + n + "_peer_gone_writes:" +
                part(InfoField::kPeerGoneWrites, i) + "\r\n";
        body += "loop" + n + "_read_pauses:" + part(InfoField::kReadPauses, i) +
                "\r\n";
        body += "loop" + n + "_accept_failures:" +
                part(InfoField::kAcceptFailures, i) + "\r\n";
        body += "loop" + n + "_cross_shard_requests:" +
                part(InfoField::kCrossShardRequests, i) + "\r\n";
        body += "loop" + n + "_pinned:" + part(InfoField::kPinned, i) + "\r\n";
      }

      body += "\r\n# Keyspace\r\n";
      for (std::size_t i = 0; i < loops; ++i) {
        body += "shard" + std::to_string(i) + "_keys:" +
                part(InfoField::kKeys, i) + "\r\n";
      }
      resp::encodeBulkString(body, out);
      break;
    }
  }
  return out;
}

}  // namespace

std::uint32_t ReplySlots::reserve() {
  const std::uint32_t slot = next_++;
  slots_.emplace_back(std::monostate{});
  return slot;
}

// slots_ is popped from the front as replies go out, so the number a slot
// carries and its position in the deque drift apart. base_ is the offset
// between them.
Slot* ReplySlots::at(std::uint32_t slot) {
  if (slot < base_) return nullptr;  // already flushed and dropped
  const std::size_t index = slot - base_;
  if (index >= slots_.size()) return nullptr;
  return &slots_[index];
}

void ReplySlots::fill(std::uint32_t slot, std::string resp) {
  if (Slot* s = at(slot); s != nullptr) *s = std::move(resp);
}

void ReplySlots::beginAggregate(std::uint32_t slot, Aggregate aggregate) {
  if (Slot* s = at(slot); s != nullptr) *s = std::move(aggregate);
}

void ReplySlots::contribute(std::uint32_t slot, std::uint32_t index,
                            std::optional<std::string> part) {
  Slot* s = at(slot);
  if (s == nullptr) return;
  auto* aggregate = std::get_if<Aggregate>(s);
  if (aggregate == nullptr) return;

  if (index < aggregate->parts.size()) aggregate->parts[index] = std::move(part);
  finishIfComplete(*s);
}

void ReplySlots::contributeCount(std::uint32_t slot, std::int64_t count) {
  Slot* s = at(slot);
  if (s == nullptr) return;
  auto* aggregate = std::get_if<Aggregate>(s);
  if (aggregate == nullptr) return;

  aggregate->accumulator += count;
  finishIfComplete(*s);
}

void ReplySlots::finishIfComplete(Slot& slot) {
  auto* aggregate = std::get_if<Aggregate>(&slot);
  if (aggregate == nullptr) return;
  if (aggregate->remaining == 0) return;

  if (--aggregate->remaining == 0) {
    // Encode before overwriting: the aggregate is the source of the string.
    slot = encodeAggregate(*aggregate);
  }
}

// The one place that decides what goes on the wire, which is why the ordering
// invariant is checkable by reading one function rather than every handler.
void ReplySlots::takeReadyPrefix(Buffer& out) {
  while (!slots_.empty()) {
    const auto* ready = std::get_if<std::string>(&slots_.front());
    if (ready == nullptr) break;  // a gap; everything behind it waits
    out.append(*ready);
    slots_.pop_front();
    ++base_;
  }
}

bool ReplySlots::idle() const { return slots_.empty(); }

std::size_t ReplySlots::pendingForTest() const { return slots_.size(); }

}  // namespace shardkv
