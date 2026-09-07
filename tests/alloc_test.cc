// Test cases C25-C32 from task.md.
//
// The product document claims the request path performs no per-request heap
// allocation for common commands. This file is that sentence turned into
// something a machine checks, because a sentence in a document drifts and an
// assertion does not.
//
// Five ways a test like this passes while the claim is false, all of them
// guarded against below:
//
//   - Short string optimisation. A command name and a small reply live inside
//     their std::string and never reach the heap, so a test written with small
//     values measures libstdc++'s buffer rather than this server's design.
//     Hence a matched pair of cases, one either side of that threshold.
//   - Warm-up. The first pass through any path pays for static initialisation,
//     lazy allocation and first growth. Counted from cold, that is noise on top
//     of the signal; the counter is therefore zeroed after a warm-up run and
//     every fixture is built before it is zeroed.
//   - The sanitizer runtimes allocate on their own account, which swamps the
//     signal. This suite is registered only for builds without one, and says so
//     out loud below rather than being quietly absent.
//   - A meta-test that does not itself allocate. "A known allocation moves the
//     counter" proves nothing if the known allocation is a short string. The
//     one here is deliberately far past any small-buffer threshold.
//   - Growth-only assertions. Comparing N against 2N catches an implementation
//     that allocates per request; it does not catch one that allocates a fixed
//     number of times per request while an allocator's free list hides the
//     difference. The steady-state count is asserted absolutely as well.
//
// And the scope, which is narrower than the claim's wording and narrower for a
// reason the architecture states: a cross-shard request MUST copy its keys,
// because the read buffer they point into is reused before the reply comes
// back. Keys are spread across shards by hash, so at eight shards seven of
// every eight single-key requests take that path. The claim can only ever have
// been about a request whose key is local, and case C31 pins that down so the
// local path's green light is never mistaken for the whole path's.

#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "commands/dispatch.h"
#include "commands/router.h"
#include "net/reply_slots.h"
#include "store/clock.h"
#include "store/hash.h"
#include "store/shard.h"
#include "helpers.h"

using namespace shardkv;
using shardkv::testing::kNoConnections;

// The suite is meaningless under a sanitizer, so it must not be built under
// one. CMake enforces that; this is the second lock, because a test that
// silently measures the wrong thing is worse than one that is missing.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#error "alloc_test measures allocations and cannot run under a sanitizer runtime"
#endif
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#    error "alloc_test measures allocations and cannot run under a sanitizer runtime"
#  endif
#endif

namespace {

std::size_t g_allocations = 0;

std::size_t allocations() { return g_allocations; }

}  // namespace

// Counted rather than intercepted: every allocation still happens exactly as it
// would in the server. Only the tally is new.
void* operator new(std::size_t n) {
  ++g_allocations;
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

// Beyond any small-string buffer libstdc++ has ever had, so a case that turns
// on "this is large" cannot be quietly reclassified by a library update.
constexpr std::size_t kBeyondSso = 256;

std::string longValue(char fill = 'v') { return std::string(kBeyondSso, fill); }

// Owns every shard and drops what is sent elsewhere. Dropping is the point: by
// the time send() is called the copies the design requires have already been
// made, and that is what is being counted. A router that kept them would be
// counting its own bookkeeping too.
class CountingRouter final : public ShardRouter {
 public:
  CountingRouter(std::size_t shards, std::size_t local, const Clock& clock) : local_(local) {
    for (std::size_t i = 0; i < shards; ++i) shards_.push_back(std::make_unique<Shard>(clock));
  }

  std::size_t shardCount() const override { return shards_.size(); }
  std::size_t localShard() const override { return local_; }
  Shard& local() override { return *shards_[local_]; }

  // Records what was sent and answers it, rather than dropping it.
  //
  // Dropping was the first version and it made the remote case measure the
  // wrong thing: a slot that is never filled is never drained, so a thousand
  // requests left a thousand live slots, the ring doubled its way up to hold
  // them, and the growth was indistinguishable from the argument copies the
  // case is actually about. Answering keeps the queue one deep, which is also
  // what a real loop does.
  void send(std::size_t shard, CrossShardRequest request) override {
    ++sent_;
    last_shard_ = shard;
    last_slot_ = request.slot;
    answered_ = true;
  }

  bool takeAnswered(std::uint32_t* slot) {
    if (!answered_) return false;
    answered_ = false;
    *slot = last_slot_;
    return true;
  }

  std::size_t sent() const { return sent_; }
  Shard& shard(std::size_t i) { return *shards_[i]; }

 private:
  std::size_t local_ = 0;
  std::size_t sent_ = 0;
  std::size_t last_shard_ = 0;
  std::uint32_t last_slot_ = 0;
  bool answered_ = false;
  std::vector<std::unique_ptr<Shard>> shards_;
};

// One trip through the request path: reserve a slot, dispatch, drain the reply.
// Everything it touches is built by the caller beforehand, so what this counts
// is the path and not the scaffolding.
struct Harness {
  ShardRouter* router = nullptr;
  ReplySlots slots;
  Buffer drain;

  // If the command went to another shard, its reply is delivered here before
  // the queue is drained. Without that the slot stays open, the queue grows
  // without bound, and what gets counted is the ring doubling rather than the
  // request path.
  CountingRouter* counting = nullptr;

  void once(const std::vector<std::string_view>& argv) {
    const std::uint32_t slot = slots.reserve();
    dispatch(*router, slots, slot, /*conn_id=*/1, argv, kNoConnections);
    std::uint32_t answered = 0;
    if (counting != nullptr && counting->takeAnswered(&answered)) {
      slots.fill(answered, "$-1\r\n");
    }
    slots.takeReadyPrefix(drain);
    drain.consume(drain.size());
    // The queue must be back to empty, or the next iteration is measuring a
    // queue that is getting deeper rather than a request that is being served.
    ASSERT_EQ(slots.pendingForTest(), 0u) << "a slot was left open";
  }

  // Runs the path `iterations` times and returns how many allocations that
  // took, having first warmed every path it will touch.
  std::size_t countOver(const std::vector<std::string_view>& argv, std::size_t iterations) {
    for (std::size_t i = 0; i < 64; ++i) once(argv);   // warm-up, uncounted
    const std::size_t before = allocations();
    for (std::size_t i = 0; i < iterations; ++i) once(argv);
    return allocations() - before;
  }
};

std::vector<std::string_view> viewsOf(const std::vector<std::string>& parts) {
  std::vector<std::string_view> argv;
  argv.reserve(parts.size());
  for (const auto& part : parts) argv.emplace_back(part);
  return argv;
}

// A key the given shard owns, found by asking the same function the server asks.
std::string keyOwnedBy(std::size_t shard, std::size_t shards) {
  for (int i = 0; i < 100000; ++i) {
    std::string candidate = "k" + std::to_string(i);
    if (shardForKey(candidate, shards) == shard) return candidate;
  }
  ADD_FAILURE() << "no key found for shard " << shard;
  return {};
}

}  // namespace

// C25 -- the meta-test. Without it, a counter that was never wired up would let
// every case below pass. Deliberately far past any small-buffer threshold: a
// meta-test built on a short string does not itself allocate.
TEST(Allocations, TheCounterIsActuallyWiredUp) {
  const std::size_t before = allocations();
  std::string big(kBeyondSso, 'x');
  EXPECT_GT(allocations(), before) << "the operator new override is not being used";
  EXPECT_EQ(big.size(), kBeyondSso);
}

// C26 -- the harness itself must be quiet, or every figure below is its noise.
TEST(Allocations, TheHarnessIsSteadyOnceWarm) {
  ManualClock clock;
  CountingRouter router(1, 0, clock);
  Harness h;
  h.router = &router;
  const auto parts = std::vector<std::string>{"PING"};
  const auto argv = viewsOf(parts);

  const std::size_t first = h.countOver(argv, 1000);
  const std::size_t second = h.countOver(argv, 1000);
  EXPECT_EQ(first, second) << "the measurement is not repeatable";
}

// C27 -- a command that touches neither store nor routing.
TEST(Allocations, PingDoesNotAllocatePerRequest) {
  ManualClock clock;
  CountingRouter router(1, 0, clock);
  Harness h;
  h.router = &router;
  const auto parts = std::vector<std::string>{"PING"};
  const auto argv = viewsOf(parts);

  const std::size_t n = h.countOver(argv, 1000);
  const std::size_t two_n = h.countOver(argv, 2000);
  EXPECT_EQ(n, 0u) << "PING allocates " << n << " times per 1000 requests";
  EXPECT_EQ(two_n, 0u);
}

// C28 -- a local read whose value fits inside its std::string.
TEST(Allocations, LocalGetOfASmallValueDoesNotAllocatePerRequest) {
  ManualClock clock;
  CountingRouter router(1, 0, clock);
  Harness h;
  h.router = &router;

  const auto seed = std::vector<std::string>{"SET", "key", "small"};
  h.once(viewsOf(seed));

  const auto parts = std::vector<std::string>{"GET", "key"};
  const auto argv = viewsOf(parts);
  const std::size_t n = h.countOver(argv, 1000);
  const std::size_t two_n = h.countOver(argv, 2000);
  EXPECT_EQ(n, 0u) << "a small local GET allocates " << n << " times per 1000 requests";
  EXPECT_EQ(two_n, 0u);
}

// C29 -- the same read with a value too large to live inside its string, and
// the case that fixes the wording of the claim rather than the code.
//
// This is C28's control, and the two disagree: a small value costs nothing and
// a large one costs exactly two allocations per request. So "a common command
// allocates nothing per request" was true of the small case only, and what made
// it true there was libstdc++'s small-string buffer rather than anything this
// server does.
//
// The two are the reply's own bytes: the shard encodes into a std::string, and
// a string longer than the small buffer must take a heap block to hold it and
// give it back when the slot is drained. Removing them means encoding straight
// into the connection's write buffer, which is a different shape of program --
// dispatch would stop returning a reply and start writing one -- and would
// reach into the ordered-slot design that
// docs/adr/0006-replies-are-written-into-ordered-slots.md sets out. That is not
// a tidy-up; it is a redesign, and it is not being done here.
//
// So the number is asserted rather than wished away. If it ever changes, this
// case says so, and the claim in docs/product.md is written to match it.
TEST(Allocations, LocalGetOfALargeValueCostsTwoAllocationsForTheReplyItself) {
  ManualClock clock;
  CountingRouter router(1, 0, clock);
  Harness h;
  h.router = &router;

  const auto seed = std::vector<std::string>{"SET", "key", longValue()};
  h.once(viewsOf(seed));

  const auto parts = std::vector<std::string>{"GET", "key"};
  const auto argv = viewsOf(parts);
  const std::size_t n = h.countOver(argv, 1000);
  EXPECT_EQ(n, 2000u) << "a large local GET allocates " << n
                      << " times per 1000 requests, not the two per request the "
                         "reply's own bytes account for";

  // And the small case is genuinely zero, so this really is the boundary rather
  // than a difference of degree.
  const auto small_seed = std::vector<std::string>{"SET", "small", "v"};
  h.once(viewsOf(small_seed));
  const auto small_parts = std::vector<std::string>{"GET", "small"};
  EXPECT_EQ(h.countOver(viewsOf(small_parts), 1000), 0u);
}

// C30 -- a write, and what it costs is stated exactly rather than allowed for.
//
// The first version of this case asserted "at most one allocation per request,
// which the stored value explains". That allowance was never approached and so
// proved nothing: overwriting a key with a value of the same length reuses the
// string already in the store and allocates nothing at all, so any OTHER
// per-request allocation could have hidden inside the allowance untouched.
// Both halves are pinned exactly instead.
TEST(Allocations, OverwritingAKeyAllocatesNothing) {
  ManualClock clock;
  CountingRouter router(1, 0, clock);
  Harness h;
  h.router = &router;

  const auto parts = std::vector<std::string>{"SET", "key", longValue()};
  const auto argv = viewsOf(parts);

  // Same key, same length, every time: the stored string is written over in
  // place. Zero, not "at most one" -- and if the request path ever starts
  // allocating for something else, this is the case that notices.
  EXPECT_EQ(h.countOver(argv, 1000), 0u);
}

// C30b -- and storing something genuinely new does allocate, which is what
// makes the case above a statement about the path rather than about a counter
// that stopped working.
TEST(Allocations, StoringNewKeysAllocatesForWhatIsStored) {
  ManualClock clock;
  CountingRouter router(1, 0, clock);
  Harness h;
  h.router = &router;

  // Built before the count starts, so what is measured is the storing and not
  // the making of the arguments.
  constexpr std::size_t kWrites = 1000;
  std::vector<std::vector<std::string>> commands;
  commands.reserve(kWrites);
  for (std::size_t i = 0; i < kWrites; ++i) {
    commands.push_back({"SET", "fresh" + std::to_string(i), longValue()});
  }
  std::vector<std::vector<std::string_view>> argvs;
  argvs.reserve(kWrites);
  for (const auto& c : commands) argvs.push_back(viewsOf(c));

  h.once(viewsOf(std::vector<std::string>{"PING"}));  // warm the path itself

  const std::size_t before = allocations();
  for (const auto& argv : argvs) h.once(argv);
  const std::size_t n = allocations() - before;

  // At least one per write for the value itself, plus the table's own nodes and
  // its rehashing. The point is not the exact figure -- it is that the counter
  // sees storage, so a zero from the overwrite case above means something.
  EXPECT_GE(n, kWrites) << "storing " << kWrites
                        << " new keys took only " << n << " allocations";
}

// C31 -- the boundary the claim was never able to cross. At more than one shard
// most single-key requests are not local, and those copy their arguments by
// design. Pinned here so the local path's silence is never read as the whole
// path's.
TEST(Allocations, ARemoteSingleKeyRequestAllocatesAndIsOutsideTheClaim) {
  ManualClock clock;
  constexpr std::size_t kShards = 8;
  CountingRouter router(kShards, 0, clock);
  Harness h;
  h.router = &router;

  const std::string remote = keyOwnedBy(3, kShards);
  ASSERT_FALSE(remote.empty());
  ASSERT_NE(shardForKey(remote, kShards), 0u) << "this key is not remote after all";

  h.counting = &router;
  const auto parts = std::vector<std::string>{"GET", remote};
  const auto argv = viewsOf(parts);
  const std::size_t n = h.countOver(argv, 1000);

  // Every request pays, and the queue never grows -- so this is the copy the
  // design requires, not the reply queue expanding behind it.
  EXPECT_GE(n, 1000u) << "a remote single-key request was expected to copy its arguments";
  EXPECT_GT(router.sent(), 0u) << "nothing was actually routed off this shard";
  EXPECT_EQ(h.slots.pendingForTest(), 0u);
}

// C32 -- and the local path at a realistic shard count, which is the only shape
// the claim can honestly describe.
TEST(Allocations, ALocalSingleKeyRequestAtEightShardsIsTheClaimsRealScope) {
  ManualClock clock;
  constexpr std::size_t kShards = 8;
  CountingRouter router(kShards, 0, clock);
  Harness h;
  h.router = &router;

  const std::string local = keyOwnedBy(0, kShards);
  ASSERT_FALSE(local.empty());

  h.once(viewsOf({"SET", local, "small"}));

  const auto parts = std::vector<std::string>{"GET", local};
  const auto argv = viewsOf(parts);
  const std::size_t n = h.countOver(argv, 1000);
  EXPECT_EQ(n, 0u) << "even the local path allocates " << n << " times per 1000 requests";
}
