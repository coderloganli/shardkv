# Use the standard library hash table first

summary: Each shard holds a `std::unordered_map` in v1; a hand-written open-addressing table is considered only once there is a baseline to compare it against.

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
