# Use the standard library hash table first

summary: Each shard holds a `std::unordered_map`; the hand-written open-addressing table this deferred is now decided against, because the profile that was to settle it puts the table nowhere near the top and the request path's time in the kernel where no table can reach it.

## Context

A key-value store's hash table is on every request path, and `std::unordered_map`
is known to be a poor performer: the standard mandates bucket iteration
semantics that force a node-based design, so every entry is a separate
allocation and a lookup chases pointers into cold cache lines. An open-addressing
table with keys and values stored inline would very likely be faster.

Writing that table is also one of the things this project exists to demonstrate.
So the temptation is to write it immediately.

The counter-pressure is that the project has fourteen days of budget, the shard
is not the only thing being built in the first six of them, and a hand-written
table is where correctness bugs are cheapest to introduce and most expensive to
find.

## Decision

Each shard holds one `std::unordered_map<std::string, Value>` for v1.

The table is reached only through the shard's own interface, never directly from
the command implementations, so that replacing it later touches one file.

Replacing it is a candidate for the measurement step, not before, and only if the
profile says the table is where the time goes.

## Reasoning

**A replacement written now would have nothing to be compared against.** The
value of a hand-written table here is not that it is faster in the abstract; it
is being able to say by how much, on this workload, against a named baseline.
Writing it first destroys the baseline and turns a measurable result into an
assertion. Writing it second costs one afternoon and produces a number.

**It is not obviously the bottleneck.** On the local-shard path a request touches
the socket, the parser, the table and the encoder. Which of those dominates is
unknown until it is profiled, and the plausible answer at this scale is the
syscalls rather than the lookup. Optimising the table first would be choosing the
target by intuition, which is the habit this project is meant to argue against.

**The alternative considered was writing the open-addressing table immediately.**
Rejected for the two reasons above, and for a third: it would put a
freshly-written data structure underneath a freshly-written event loop and a
freshly-written protocol parser, so a wrong answer in the first week would have
three plausible sources instead of two.

Note that `std::hash` is not used for the keys even in v1 — see the decision on
the hash function. The table implementation and the hash function are separate
choices, and only the table is being deferred.

---

## Resolved, 2026-09-07: the table stays

This record deferred a decision rather than making one: write the
open-addressing table "once there is a baseline to compare it against". There is
now a profile, and it settles it. **The table is not replaced.**

**What the profile says.** Sampling this server at eight shards under a
saturating load, the ranked user-space cost is `recv`, `__send`, `read`, `write`
and `epoll_pwait` — the system-call wrappers, better than half of what the
profile can see. The store's lookup does not appear near the top at all. That is
what this record guessed when it wrote "the plausible answer at this scale is the
syscalls rather than the lookup", and the guess is now a measurement.

**And the ranking covers less than a third of the cost.** The recording sees
user-mode time only (`0017-the-profile-sees-user-space-only.md`), and the same
run recorded 13.1 seconds of user CPU against 31.6 in the kernel. Where that
kernel time went is not attributed by this profile and is not claimed here — the
figure is a total. What can be said is that the part the ranking above covers is
under a third of the whole, and a faster table competes for a slice of that
smaller share.

**Why this is a result and not an excuse.** The reasoning above was written
before any of it could be checked, and it could have come out the other way; the
table could have been at the top of the list and this record would then have
called for writing it. Deferring produced a decision backed by a measurement,
which is what deferring was for. Writing the table first would have produced a
faster table and no way to say by how much it mattered.

**What was worth changing instead, and it was not visible in the profile
either.** An allocation counter — not a profiler — found the reply queue taking
a heap block every sixth command, which was the whole of the gap between this
server and its own published claim about allocation. That is recorded in
`0019-the-reply-queue-is-a-ring-not-a-deque.md`. The profiler's own symbols
(`malloc`, `cfree`) were visible the whole time and said nothing about what the
allocations were for.

**What would reopen this.** A workload where the store is reached far more often
per system call than one command per read — heavy pipelining, or multi-key
commands over large batches — moves the balance, and the profile would show it.
The interface that made replacing the table cheap is still the interface, so the
option this record preserved is still open. It is simply not taken.
