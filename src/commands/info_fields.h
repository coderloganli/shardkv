#pragma once

#include <cstdint>

namespace shardkv {

// Which block of a kInfo aggregate's parts a value belongs to.
//
// The parts vector is flat -- field * loops + loop -- because the slot
// machinery counts contributions and has no opinion about what they mean. Kept
// in its own header so reply_slots.h can lay the parts out without depending on
// the command layer.
enum class InfoField : std::uint32_t {
  kKeys = 0,
  kConnections,
  kShortWrites,
  kPeerGoneWrites,
  kCrossShardRequests,
  kPinned,
  kCount,
};

}  // namespace shardkv
