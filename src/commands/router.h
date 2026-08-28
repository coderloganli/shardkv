#pragma once

#include <cstddef>

#include "net/message.h"
#include "store/shard.h"

namespace shardkv {

// How command execution reaches shards.
//
// local() is the ONLY way to touch a shard directly, and it is always this
// loop's own. Everything else is a message. The rule that a thread touches only
// its own partition is therefore the type system's job rather than a convention
// reviewers have to enforce -- there is no handle to another shard to misuse.
//
// It is abstract so that tests can hold cross-shard requests and release them
// when they choose, which is what makes the reply-ordering rules testable
// without threads or sockets. See
// docs/adr/0008-routing-is-an-interface-so-ordering-can-be-tested.md
class ShardRouter {
 public:
  virtual ~ShardRouter() = default;

  virtual std::size_t shardCount() const = 0;
  virtual std::size_t localShard() const = 0;
  virtual Shard& local() = 0;

  // Takes ownership of the request.
  virtual void send(std::size_t shard, CrossShardRequest request) = 0;
};

}  // namespace shardkv
