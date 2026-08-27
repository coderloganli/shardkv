# Hash keys with xxHash, not `std::hash`

summary: Keys are hashed with xxHash (XXH3, vendored as a single header) for both the shard mapping and the table; `std::hash` is not used for strings.

## Context

Two different things need a hash of the key: the hash table inside a shard, and
`shard = hash(key) % N`, which decides which thread owns the key at all.

The second one is what makes the choice matter. If the hash does not avalanche —
if related keys map to related hash values — then a workload with structured key
names, which is every real workload (`user:1`, `user:2`, `session:abc`), piles
disproportionately onto a subset of shards. One thread saturates while the others
idle, and the multi-core throughput claim quietly stops being true. The failure
mode is not a wrong answer; it is a benchmark result that looks like the
architecture does not work.

`std::hash<std::string>` gives no avalanche guarantee. It is not specified beyond
being a hash, and its quality varies between standard library implementations:
what libstdc++ does and what libc++ or MSVC do are different functions with
different distribution properties. A shard-balance bug that reproduces on one
machine and not another is the worst class of bug this project could ship.

## Decision

Keys are hashed with xxHash (the XXH3 variant), vendored into the repository as
its single public-domain header.

The same function serves both uses: the shard mapping and the hash table's
hasher.

## Reasoning

**It is one file, so it does not breach the no-dependencies principle.** That
principle exists so that the parts the project is meant to demonstrate — the
networking, the concurrency, the data structures — are written rather than
imported. A hash function is none of those. Vendoring a header is closer to
copying a constant than to taking a dependency: there is no build system
integration, no version to track, and nothing to link.

**Avalanche is the property being bought**, and it is the one `std::hash` does
not promise. XXH3 passes SMHasher, which is the test suite that exists to
demonstrate exactly this.

**Speed is a secondary benefit, not the reason.** XXH3 is fast on short inputs,
which is what keys are, but a slower hash with good distribution would still have
been chosen over a faster one without it.

**The alternative considered was wyhash**, which is comparable in quality and
speed and is also a single file. xxHash was chosen for having the more
established track record and the wider deployment; on the merits the two were
close enough that either would have been defensible.

**`std::hash` was rejected** for the unspecified-quality reason above. Writing a
hash function by hand was not seriously considered: it would be the one piece of
hand-written code in this project whose failure mode is a subtly skewed
distribution rather than a visible bug.

Note that this decision is independent of which hash *table* is used — see the
decision to keep `std::unordered_map` for v1. The table is deferred; the hash
function is not, because getting it wrong distorts the measurements the project
is built to produce.
