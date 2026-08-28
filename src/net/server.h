#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <thread>

#include "net/loop.h"
#include "store/clock.h"
#include "store/shard.h"

namespace shardkv {


// Parses a --shards argument. Rejects anything that is not a whole positive
// number consumed in full, so "4x" is an error rather than 4 -- a benchmark
// launched with a typo must not quietly run something else. Declared here
// rather than hidden in main.cc so the rule can be tested directly.
std::optional<std::size_t> parseShardCountForTest(std::string_view text);

// The shard count when --shards is not given: one per core.
//
// std::thread::hardware_concurrency() is permitted by the standard to return 0
// when it cannot tell, and 0 is not a shard count. The floor lives here, in one
// place that a test can call, rather than inline in main() where the rule would
// be asserted by restating it.
std::size_t defaultShardCount();

// N shards, N loops, N threads, all sharing one port through SO_REUSEPORT.
//
// STAGE 6 STUB: declarations only.
class Server {
 public:
  struct Options {
    std::uint16_t port = 6380;
    // max(1u, hardware_concurrency()) by the time it reaches here.
    // hardware_concurrency is permitted by the standard to return 0, which is
    // not a shard count.
    std::size_t shards = 1;
    bool pin = false;
  };

  Server(Options options, const Clock& clock);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  std::uint16_t port() const;
  std::size_t shardCount() const;

  // Starts the threads and returns.
  void start();

  // Two-phase, and the order is the point. Stopping loops one at a time and
  // letting each drain its own inbox leaks exactly what it is trying to
  // protect: a loop that has already exited can still be sent to by one that
  // has not. So: publish stopping so no loop produces more work, let every loop
  // exit, join them all, and only then drain and free the inboxes, when there
  // is no producer left.
  //
  // Safe to call twice.
  void stop();

 private:
  Options options_;
  const Clock* clock_ = nullptr;
  std::vector<std::unique_ptr<Shard>> shards_;
  std::vector<std::unique_ptr<Loop>> loops_;
  std::vector<std::thread> threads_;
  LoopTable table_;
  std::uint16_t port_ = 0;
  bool stopped_ = false;
};

}  // namespace shardkv
