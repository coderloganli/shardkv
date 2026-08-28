# A lock-free MPSC queue carries cross-shard messages

summary: Each loop has one intrusive multi-producer single-consumer queue built on atomics with explicit acquire/release ordering; an eventfd wakes the consumer, and the write is elided when the queue was already non-empty.

## Context

Every loop needs an inbox that any other loop can push to and only it pops from:
multi-producer, single-consumer. It carries cross-shard requests inbound and
cross-shard replies back.

A mutex-protected `deque` would do the job and is very hard to get wrong. The
alternative is a lock-free queue: atomics, and explicit memory ordering at every
step.

## Decision

An intrusive MPSC queue of the Vyukov shape: producers exchange a tail pointer,
the consumer walks a stub-terminated list. Every atomic operation carries an
explicit memory order and a comment saying what it is ordering and why —
`memory_order_relaxed` is never used because it is faster, only where there is
genuinely nothing to order.

A consumer is woken by writing 1 to its eventfd. **The write is skipped when the
producer's exchange showed the queue was already non-empty**, since a consumer
that has not yet drained is already awake or already has a pending wakeup.

## Reasoning

**This is one of the things the project exists to demonstrate.** The capability
being claimed is not "used a queue"; it is understanding what the cores are
doing to each other and being able to say which memory ordering makes the
handover safe and why a weaker one would not. A mutex here would be a correct
program that demonstrates nothing about that, and the reasoning would be
unavailable to talk about because it would live inside libstdc++.

**It also keeps the architecture's own claim honest.** The design says no data
is shared between threads and the only channel between them is a message queue.
A mutex would be shared mutable state introduced on the very path that exists to
avoid it. Not fatal, but it would need explaining every time the design is
described, and a design that needs a footnote every time is a design with a seam
in it.

**The risk is real and is accepted with eyes open.** This is the code in the
project most able to hide a subtle bug, and the tooling is uneven: ThreadSanitizer
checks the data races, which is the part most likely to be wrong, but nothing
checks for a lost wakeup — a queue left non-empty with a consumer asleep. That
failure does not corrupt anything; it hangs, and only under a particular
interleaving. So the wakeup rule gets a test of its own that hammers the
empty-to-non-empty transition, rather than resting on inspection.

**That risk then materialised, which is worth recording rather than tidying
away.** The first working version lost wakeups about one run in twelve. The
sequence: a producer swaps itself onto the tail and, before it can link itself
in, the consumer pops, finds the last linked node's `next` still null, sees the
tail has moved past it, and returns nothing. The consumer reads that as an empty
queue and sleeps — but the producer behind the one in the window saw a non-empty
queue and decided not to signal, and now nobody will.

The fix is that `pop()` distinguishes the two reasons for returning nothing: the
queue is empty, or a producer is mid-push and there is work here. A consumer may
sleep on the first and must not on the second.

ThreadSanitizer was clean throughout: there was no race, only a consumer asleep
beside a queue with something in it. It was found by a test timing out.

**The alternative sequence considered was: mutex first, replace after
measurement**, mirroring the reasoning that kept `std::unordered_map` for v1 and
kept integers out of `Value`. It was rejected because those two are optimisations
of a thing that already works, deferred for want of a baseline, whereas this is
the mechanism itself. There is no version of this project that ships a
cross-core message channel and has nothing to say about how it is synchronised.

## The wakeup rule, stated exactly

`read()` on an eventfd without `EFD_SEMAPHORE` returns the counter and resets it
to zero, and epoll reports the fd readable whenever the counter is above zero
(`eventfd(2)`). So a single `read()` drains any number of coalesced wakeups, and
the consumer must drain the *queue* to empty rather than assuming one wakeup
means one message.

The ordering that makes eliding the write safe: a producer publishes its node
with a release store and only then decides whether to signal; a consumer drains
until the queue is observably empty and only then blocks. A wakeup can therefore
be redundant but cannot be missing.

The counter cannot overflow in this use — its maximum is
`0xfffffffffffffffe`, each wakeup adds one, and every drain resets it to zero.
