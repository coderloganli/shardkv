# Product design

## What this product is

shardkv is a concurrent in-memory key-value store that speaks the Redis wire
protocol (RESP2). Existing Redis clients and tools connect to it unmodified:
`redis-cli` for interaction, `redis-benchmark` for load.

What distinguishes it from the server it imitates is the concurrency model.
Redis serves commands from a single thread. shardkv runs one epoll event loop
per core, partitions the keyspace across those cores, and lets each core serve
its own share in parallel. Nothing mutable is shared between threads, so there
is no global lock anywhere on the request path.

## Who it is for

Two audiences, and they want different things.

**Someone reading the source.** They are here to see how a shared-nothing
network server is built end to end: how connections are distributed across
threads without a load balancer, how a keyspace is partitioned so that no lock
is ever needed, what the cost of that choice is, and how the claim that no two
threads touch the same data gets verified rather than asserted. The code and the
decision records are the product for this reader.

**Someone running it.** They want a fast local key-value store with a protocol
they already have clients for. They get parallelism across cores that
single-threaded Redis does not offer, at the price of a much smaller feature set.

## What it does

The v1 command set is a core subset, chosen so that `redis-cli` and
`redis-benchmark` work without modification:

| Group        | Commands                                                  |
|--------------|-----------------------------------------------------------|
| Connectivity | `PING` `ECHO` `QUIT` `COMMAND`                             |
| Strings      | `SET` `GET` `GETSET` `APPEND` `STRLEN`                     |
| Counters     | `INCR` `DECR` `INCRBY` `DECRBY`                            |
| Multi-key    | `MGET` `MSET` (cross-shard scatter/gather)                 |
| Keys         | `DEL` `EXISTS` `TYPE` `DBSIZE` `FLUSHDB`                   |
| Expiry       | `EXPIRE` `TTL` `PERSIST` `SET key val EX n`                |
| Ops          | `INFO`, reporting per-shard key counts and per-loop connection counts |

`COMMAND` is implemented, returning an empty array, for client compatibility.

It is sometimes said that a recent `redis-cli` sends `COMMAND DOCS` on connect
and hangs without a reply. That claim is not in the official documentation, and
testing it here did not reproduce it: with `COMMAND` answered as an unknown
command, `redis-cli` 7.0.15 connected and ran commands normally, over a piped
session and a single command alike. `COMMAND` is kept because it costs nothing
and clients may reasonably ask for it -- not because anything is known to break
without it. A genuinely interactive session on a terminal, where the client
fetches syntax hints, was not exercised and remains the one case that could
still behave differently.

`redis-benchmark` prints `WARNING: Could not fetch server CONFIG` and then runs
normally. `CONFIG` is outside the v1 command set; the warning is cosmetic.

Candidates for v2, if time allows: `SETNX`, `SETEX`, `KEYS`, `SCAN`,
`RANDOMKEY`, and the hash type (`HSET` / `HGET` / `HDEL`).

Both RESP2 request forms are accepted: arrays of bulk strings, which is what
real clients send, and inline commands such as a bare `PING\r\n`, which is what
`telnet` and some tools send.

## What it deliberately does not do

These are boundaries, not a backlog. The goal is to demonstrate a concurrency
architecture, not to reimplement a database — and a project without stated
limits invites the question of what is missing instead of the question of what
was chosen.

- **Clustering and distribution.** Sharding here is across cores within one
  process. There is no multi-node sharding, no Raft, no failover. The word
  "shard" in the name refers to cores, not machines.
- **Persistence.** Not in v1. Everything is lost on restart. An append-only file
  is on the roadmap if time allows.
- **Replication.** No primary/replica, no `REPLICAOF`.
- **Transactions and scripting.** No `MULTI` / `EXEC`, no Lua.
- **Pub/Sub, Streams, and Cluster-family commands.**
- **A complete command set.** The table above is the whole surface.

There is also a performance boundary worth stating plainly, because the
architecture guarantees it: cross-shard commands cost an extra hop between
threads, so `MGET`, `MSET` and multi-key `DEL` are slower here than in
single-threaded Redis. Single-key commands, which are the overwhelming majority
of real traffic, are the case this design optimises for.

## Principles

**No third-party libraries**, apart from something like a hash function that can
be embedded as a single file. Networking, concurrency and data structures are
written by hand. The point of the project is those layers; importing them would
remove the thing being built.

**Linux only.** `SO_REUSEPORT`, `eventfd` and `epoll` are Linux facilities and
the design is built on all three. Portability is not a goal, and pretending
otherwise would distort the code.

**Shared-nothing over locking.** Every key belongs to exactly one thread, and
only that thread may touch it. When a request needs a key that belongs to
another thread, it is forwarded as a message rather than reaching across a lock.
The alternative — one hash table split into lock-protected segments — was
considered and rejected; the reasoning is in `docs/adr/`.

**No unmeasured performance claims.** No throughput or latency figure appears in
this repository until it has been produced by a script anyone can rerun, on a
machine described by CPU model, core count, kernel version, compiler and
optimisation level. A number without its environment is not a number.

**A red CI is the point.** Builds run under AddressSanitizer, UndefinedBehavior
Sanitizer and ThreadSanitizer. A pipeline that is always green has not
demonstrated anything.

**A race is a design failure, not a locking opportunity.** The correctness of
the whole architecture rests on the claim that no two threads touch the same
data. ThreadSanitizer is what checks that claim mechanically. If it reports a
race, the response is to find which piece of state escaped its thread — not to
add a mutex and move on.

## Where the details live

`docs/architecture.md` describes how the system is built. Decisions and the
alternatives they beat are in `docs/adr/`, one per file; search them rather than
reading the directory.
