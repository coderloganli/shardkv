# Routing is an interface, so ordering can be tested

summary: Commands reach shards through an abstract `ShardRouter`; the real one delivers to other loops, and a test one holds cross-shard requests until the test releases them, which is what makes the reply-ordering rules deterministically testable.

## Context

The hard invariant in this design is that replies leave a connection in command
order however their work completes — the reply-slot scheme in
`docs/adr/0006-replies-are-written-into-ordered-slots.md`. It has to hold under
a pipelined mix of local and cross-shard commands, with cross-shard replies
arriving in any order at all.

Testing that against a running multi-loop server means arranging for a
cross-shard reply to be slow, and knowing which loop a connection landed on.
Neither is controllable: the delay would be a `sleep` and a hope, and connection
placement is the kernel's business, deliberately not assumed anywhere in this
design.

There is recent precedent for how that goes. In the previous task, a test that
claimed to prove the write-to-a-departed-peer path was reached asserted its way
around a race and had to be rewritten twice before the honest answer emerged:
the path could not be forced from outside, and a socketpair unit test proved it
instead. Timing-based concurrency tests do not prove what they claim.

## Decision

Command execution reaches shards only through an abstract interface:

```cpp
class ShardRouter {
 public:
  virtual ~ShardRouter() = default;   // owned through a base pointer
  virtual std::size_t shardCount() const = 0;
  virtual std::size_t localShard() const = 0;
  virtual Shard& local() = 0;
  virtual void send(std::size_t shard, CrossShardRequest req) = 0;
};
```

`LoopRouter` implements it by pushing onto another loop's queue. The tests use a
`TestRouter` that owns every shard directly, executes local work, and **holds
cross-shard requests in a list until the test releases them** — individually,
in any order it likes.

So the ordering tests run with no threads and no sockets. "The remote reply has
not come back yet" is a line of code.

## Reasoning

**It converts the central invariant from something argued to something
demonstrated.** Out-of-order release, a pipeline blocked on its first command, a
terminal `QUIT` slot behind an outstanding remote reply — each becomes an
ordinary unit test with an exact expected byte string. None of them is
expressible against a live server without timing assumptions.

**It also isolates the interface the architecture already claims.** The rule
that a loop touches only its own shard was going to be a convention that
reviewers enforce. As an interface it is the type system's job: there is no
handle to another shard to misuse, because `local()` is the only accessor and
everything else goes through `send()`.

**The cost is one indirect call per command**, on the request path, and it is
accepted rather than dismissed. Two reasons. It is unmeasured, and this project
does not pay for unmeasured optimisations — the same argument that kept
`std::unordered_map` (ADR 0001) and kept integers out of `Value` (ADR 0004).
And if the profile in the measurement step says it matters, devirtualising is a
local change: the interface has one production implementation, so it can become
a template parameter or a tagged dispatch without touching command code.

**The alternative was a compile-time seam** — templating command execution on the
router type, so the test double costs nothing at runtime. Rejected because the
template would spread: `Connection` holds the router, `Loop` holds connections,
and the whole net layer becomes templated on a type that has exactly one
production instantiation. That is a large, viral complication to avoid an
indirect call nobody has measured.

## Consequence

`tests/helpers.h` gains a local-only router so the existing command tests keep
their bodies unchanged — they construct a router over one shard and read the
reply out of a slot instead of a string. Around sixty existing test cases carry
over untouched.
