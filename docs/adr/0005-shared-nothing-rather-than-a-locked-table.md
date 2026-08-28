# Shared-nothing rather than one table under locks

summary: Every key belongs to exactly one thread and only that thread touches it; work for a key that lives elsewhere is sent as a message rather than reaching across a lock.

## Context

The keyspace has to be served by every core. There are two ways to arrange that,
and this is the decision the whole project exists to demonstrate, so it is worth
stating both properly rather than asserting the winner.

**One table, segmented locks.** A single hash table split into some number of
segments, each with its own mutex. Any thread may reach any key by taking the
right lock.

**Shared nothing.** The keyspace is partitioned across threads. A thread owns its
partition outright and no other thread may touch it. A request for a key that
belongs elsewhere is forwarded to the owning thread as a message.

Until now this has been recorded only as prose in `docs/architecture.md`. It is
being written down properly at the point it stops being a plan and becomes code.

## Decision

Shared nothing. `shard = xxhash(key) % N`, one shard per loop, one loop per
thread. The only channel between threads is a message queue with an eventfd
wakeup.

There is no mutex anywhere on the request path, and none inside `Loop`, `Shard`
or `Connection` — not because locks were optimised away, but because there is
nothing for them to protect.

**Precisely: no data is shared.** Keys, values, connections, buffers and reply
slots each belong to one thread for their whole lifetime. The queues themselves
are shared by construction, and one atomic flag says the server is shutting
down. That is lifecycle machinery, not something a command touches, and the
distinction is worth keeping straight — "threads share no mutable state" is the
sentence one wants to write, and it is not true.

## Reasoning

**The costs are paid in different places, and the workload decides which is
cheaper.** Segmented locking pays on every access: an atomic operation to take
the lock whether or not anyone else wanted it. Shared nothing pays only when a
request crosses a shard: a message, a wakeup, and a copy of the key. Redis
traffic is overwhelmingly single-key — `redis-benchmark`'s default GET and SET
are — so the common path pays nothing at all, and the uncommon path pays
visibly. That is the trade, and it is a bet on the shape of the workload rather
than a claim of universal superiority.

**Lock contention is not the worst part of segmented locking; cache-line
bouncing is.** Several cores repeatedly acquiring the same mutex pass that
mutex's cache line between them, and the line moves whether or not the lock was
actually contended. Partitioning means a shard's data is read and written by one
core, so it stays in that core's cache.

**Single-threaded semantics are what make the code reviewable.** Inside a loop,
the code is ordinary single-threaded code: no lock ordering to get right, no
critical section whose boundaries someone has to keep in their head, no question
of what is safe to do while holding what. That is worth a great deal in a
codebase where the event loop and the protocol parser are also new.

**And it is machine-checkable.** This is the argument that decides it. The
correctness of the whole arrangement reduces to one claim — no two threads touch
the same data — and ThreadSanitizer checks exactly that claim, mechanically, on
every CI run. A segmented-lock design has no equivalent: TSan can find a missing
lock, but nothing can tell you the locking scheme is right. **If TSan reports a
race here, it means a piece of state escaped its thread, and the answer is to
find which one — never to add a mutex and move on.**

## The costs, stated plainly

**Cross-shard commands are slower than single-threaded Redis on that path.** An
extra hop between threads, plus a copy of the key, because the originating
connection's read buffer may be reused before the reply comes back. `MGET`,
`MSET` and multi-key `DEL` pay this.

**A cross-shard `MSET` is not atomic.** Its writes land on different threads at
different moments, so a concurrent `MGET` can observe half of one. Redis's `MSET`
is atomic; this is a real behavioural difference and it is recorded as a
non-goal rather than hidden.

**Connection distribution is the kernel's business.** Each loop accepts on its
own `SO_REUSEPORT` socket, so how connections spread across loops is not
something this design controls -- see the note in `docs/architecture.md` about
what `socket(7)` does and does not promise.
