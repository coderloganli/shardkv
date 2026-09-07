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

Slot& ReplySlots::atIndex(std::size_t i) {
  return ring_[(head_ + i) % ring_.size()];
}

const Slot& ReplySlots::atIndex(std::size_t i) const {
  return ring_[(head_ + i) % ring_.size()];
}

// Doubling, and never shrinking. A connection that once queued deeply keeps the
// room to do it again, which is the same bargain the read and write buffers
// strike: the memory is bounded by the deepest burst the connection ever had,
// and in exchange the steady state allocates nothing at all.
void ReplySlots::grow() {
  const std::size_t wanted = ring_.empty() ? 8 : ring_.size() * 2;
  std::vector<Slot> next(wanted);
  for (std::size_t i = 0; i < size_; ++i) next[i] = std::move(atIndex(i));
  ring_.swap(next);
  head_ = 0;
}

std::uint32_t ReplySlots::reserve() {
  if (size_ == ring_.size()) grow();
  const std::uint32_t slot = next_++;
  atIndex(size_) = std::monostate{};
  ++size_;
  return slot;
}

// The queue is drained from the front as replies go out, so the number a slot
// carries and its position in the ring drift apart. base_ is the offset between
// them.
Slot* ReplySlots::at(std::uint32_t slot) {
  if (slot < base_) return nullptr;  // already flushed and dropped
  const std::size_t index = slot - base_;
  if (index >= size_) return nullptr;
  return &atIndex(index);
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
  while (size_ > 0) {
    Slot& front = atIndex(0);
    const auto* ready = std::get_if<std::string>(&front);
    if (ready == nullptr) break;  // a gap; everything behind it waits
    out.append(*ready);
    // Reset rather than merely step over: the slot holds the reply's bytes, and
    // a ring that keeps its cells would hold every reply this connection has
    // ever sent.
    front = std::monostate{};
    head_ = (head_ + 1) % ring_.size();
    --size_;
    ++base_;
  }
}

bool ReplySlots::idle() const { return size_ == 0; }

std::size_t ReplySlots::pendingForTest() const { return size_; }

}  // namespace shardkv
