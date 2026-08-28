#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace shardkv {

// A command to run on another loop's shard.
//
// argv holds OWNED copies, never views. The originating connection's read
// buffer may be reused long before this reply comes back, so borrowing from it
// is the same mistake the parser's lifetime rule exists to prevent -- see
// proto/parser.h. This copy is the concrete price of the shared-nothing design,
// paid only on the cross-shard path.
// How the answer to a request should be delivered into the waiting slot. A
// message has to say this: the loop that receives the reply has long since
// forgotten what command asked for it.
enum class Delivery {
  kWhole,       // a complete RESP reply for a single-key command
  kArrayParts,  // one element per key, at its original argument position
  kCount,       // a number to add to a running total
  kStatus,      // nothing but "this group is done"
  // INFO: this loop's key count and connection count. Answered by the loop
  // itself -- one of the two numbers is not the shard's to give.
  kLoopInfo,
};

struct CrossShardRequest {
  std::size_t origin_loop = 0;
  std::uint64_t conn_id = 0;
  std::uint32_t slot = 0;
  // Which group of a scattered multi-key command this is; 0 for single-key.
  std::uint32_t group = 0;
  Delivery delivery = Delivery::kWhole;
  std::vector<std::string> argv;
  // For kArrayParts: where each of this group's keys sat in the original
  // command, so the reply can be assembled in the order the client wrote it
  // rather than the order the shards happened to answer.
  std::vector<std::uint32_t> indices;
};

// The answer, travelling back to the loop that asked.
//
// Addressed by (origin_loop, conn_id) rather than by pointer: the connection
// may be gone by now, and a reply naming a connection that no longer exists is
// simply dropped. Per-loop counters are never reused, so a new connection can
// never be handed an old one's reply.
struct CrossShardReply {
  std::uint64_t conn_id = 0;
  std::uint32_t slot = 0;

  // How to put this into the waiting slot. It must travel with the reply: by
  // the time it arrives, the loop that asked has no idea which command it
  // belonged to, and guessing wrong is not a wrong answer but a hang -- an
  // aggregate overwritten by a whole reply never completes, and the connection
  // waits behind it forever.
  Delivery delivery = Delivery::kWhole;

  std::string payload;              // kWhole: the encoded RESP
  std::uint32_t index = 0;          // kArrayParts, kLoopInfo: which part
  std::optional<std::string> part;  // kArrayParts, kLoopInfo: the value there
  std::int64_t count = 0;           // kCount: this group's contribution
};

}  // namespace shardkv
