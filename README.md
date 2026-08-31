# shardkv

[![CI](https://github.com/coderloganli/shardkv/actions/workflows/ci.yml/badge.svg)](https://github.com/coderloganli/shardkv/actions/workflows/ci.yml)

A concurrent in-memory key-value store in C++20, speaking the Redis wire protocol
(RESP2). The architecture is shared-nothing: one epoll event loop per core, the
keyspace partitioned across those cores, and no global lock anywhere on the
request path.

> **Status: measured.** The server runs N event loops on N threads with the
> keyspace partitioned across them, answers the v1 command set correctly at any
> shard count, and holds its resources: buffers compact, a client that will not
> read its replies is stopped being read from, and expired keys nobody touches
> again are reaped in the background.
>
> The numbers below were produced by scripts in `benchmarks/`, on a named
> machine, against `redis-server` on that same machine, with the predictions
> written down and committed before the runs. **Two of the five predictions
> failed, and those are the interesting part.**

## The idea

```
                    +--------------------------------------+
   TCP connection --▶| SO_REUSEPORT: the kernel assigns each |
                    | connection to one listening socket   |
                    +--------------------------------------+
                        |           |           |
                +-------▼---+ +-----▼-----+ +---▼-------+
                |  Loop 0   | |  Loop 1   | | Loop N-1  |  one thread per shard
                |  epoll    | |  epoll    | |  epoll    |  (--pin adds affinity)
                |  conns    | |  conns    | |  conns    |
                |  Shard 0  | |  Shard 1  | | Shard N-1 |  1/N of the keyspace
                +-----------+ +-----------+ +-----------+
                      ▲   |         ▲   |        ▲   |
                      |   +---------+---+--------+---+
                      +-------------+------------+
                     cross-shard requests: MPSC queue + eventfd
```

Each thread owns its epoll instance, its set of connections, and one hash table
shard that no other thread may touch. No data is shared between threads -- keys,
values, connections and buffers each belong to one thread for their whole life --
and the only channel between them is a message queue.

A key that lands on the loop its connection belongs to is served without crossing
a thread, taking a lock, or allocating. A key that does not is forwarded to the
owning loop as a message. That is the trade: synchronisation cost is paid per
cross-shard request rather than per access, which pays off because the
overwhelming majority of real Redis traffic is single-key commands.

The honest cost: cross-shard commands (`MGET`, `MSET`, multi-key `DEL`) take an
extra hop and will be slower than single-threaded Redis on that path.

## Why RESP

Because the tooling already exists. `redis-cli` connects, `redis-benchmark`
generates load, and `redis-server` on the same machine is a control group — so
performance claims can be compared against something rather than asserted.

## Scope

Planned for v1:

| Group        | Commands                                              |
|--------------|-------------------------------------------------------|
| Connectivity | `PING` `ECHO` `QUIT` `COMMAND`                         |
| Strings      | `SET` `GET` `GETSET` `APPEND` `STRLEN`                 |
| Counters     | `INCR` `DECR` `INCRBY` `DECRBY`                        |
| Multi-key    | `MGET` `MSET` (cross-shard scatter/gather)             |
| Keys         | `DEL` `EXISTS` `TYPE` `DBSIZE` `FLUSHDB`               |
| Expiry       | `EXPIRE` `TTL` `PERSIST` `SET key val EX n`            |
| Ops          | `INFO` (per-shard key counts, per-loop connection counts) |

## Deliberately not built

- **Clustering and distribution.** Sharding here is across cores on one machine,
  not across nodes. No Raft, no failover.
- **Persistence.** Not in v1. AOF is on the roadmap if time allows.
- **Replication.** No primary/replica.
- **Transactions and Lua.** No `MULTI`/`EXEC`, no scripting.
- **Pub/Sub, Streams, Cluster commands.**
- **A complete command set.** A core subset only.

These are boundaries, not omissions. The goal is to demonstrate concurrency and
systems work, not to reimplement a database.

## Building

Requires Linux: epoll, `SO_REUSEPORT` and eventfd have no portable equivalents
and the design rests on all three.

```
cmake -B build && cmake --build build && ctest --test-dir build
./build/shardkv --port 6380
redis-cli -p 6380 PING
```

Port 6380 rather than 6379, so a real `redis-server` can run alongside as the
control group.

There is a `Dockerfile` carrying the toolchain, `redis-tools` and
`redis-server` — the last of these because every performance figure here is a
comparison against Redis on the same machine. It is how this is developed and how
CI runs:

```
docker build -t shardkv-dev .
docker run --rm -v "$PWD":/src -w /src shardkv-dev   bash -c 'cmake -B build && cmake --build build && ctest --test-dir build'
```

Run several loops with `--shards N` (the default is the core count) and add
`--pin` for CPU affinity, which is off by default because it does more harm than
good on a container or a shared machine.

Sanitizer builds are selected with `-DSHARDKV_SANITIZER=address` or `=thread`.
The thread build additionally needs `--security-opt seccomp=unconfined` under
Docker; `docs/architecture.md` says why.

## What it measures out at

Every figure here comes from `benchmarks/`, with its environment recorded beside
it and a script anyone can rerun. **Read the two caveats before the tables** —
they are what the figures mean.

**The machine.** AMD Ryzen 7 7800X3D, 8 cores, no SMT; Linux
6.6.87.2-microsoft-standard-WSL2 in a container on Docker Desktop; g++ 13.3.0,
`-O2` via `CMAKE_BUILD_TYPE=Release`; `redis-server` and `redis-benchmark` both
7.0.15; load generated on the same machine; threads not pinned. Eight shards.

**What is not here, and why.** There is **no scaling curve and no profile.** The
machine is virtualised and its PMU is unreachable, so a curve drawn on it would
describe the hypervisor's scheduler as much as this architecture, and
`perf`'s counters do not exist to be read. **No claim is made anywhere that
throughput scales linearly, or in any particular way, with cores.** What replaces
it is a control group under identical conditions —
`docs/adr/0014-what-this-machine-can-and-cannot-measure.md` has the reasoning.

### The load generator was the bottleneck, and finding that out was the point

The technical document specifies `redis-benchmark -t get,set -n 1000000 -c 50`.
Run exactly that and shardkv looks **slower** than Redis:

| single-key GET/SET, `-c 50` | shardkv | redis-server | ratio |
|---|---|---|---|
| one generator thread (as specified) | 43,716 ops/s | 65,011 ops/s | **0.67x** |
| generator given 8 threads | **266,383 ops/s** | 52,571 ops/s | **5.07x** |

`redis-benchmark` is single-threaded unless told otherwise. Fifty connections
through one client thread saturate that thread at around 45,000 requests a
second — which is a fact about the benchmark, not about the server. Redis, being
itself single-threaded, is matched by it; eight loops are not.

Give the generator eight threads and the picture inverts. Redis gets *slower*
(52,571 from 65,011), because now eight client threads compete with its one
server thread for eight cores. shardkv gets six times faster.

**Neither row is the "true" number.** The first measures the instrument; the
second takes cores away from the server on a machine that has only eight. Both
are reported because reporting only the first would publish the benchmark's
ceiling as this server's, and reporting only the second would look like picking
the flattering one.

### Latency, one connection, no pipelining

| p50 | p95 | p99 | p99.9 |
|---|---|---|---|
| shardkv **0.143 ms** | 0.183 | 0.231 | 0.327 |
| redis-server **0.079 ms** | 0.103 | 0.135 | 0.207 |

shardkv is **81% slower per request** here, and the prediction said "within about
30%". That failure has an explanation, and the explanation is measured rather
than guessed — see below.

### The cross-shard penalty

Each run is one connection against keys belonging to one shard, classified after
the fact by whether `cross_shard_requests` moved. 80 runs, of which 12 turned out
local and 68 remote.

| | median | min | max | samples |
|---|---|---|---|---|
| keys on the connection's own loop | **0.079 ms** | 0.079 | 0.079 | 12 |
| keys on another loop | **0.143 ms** | 0.135 | 0.151 | 68 |

**The penalty is 0.064 ms per request**, and the two groups' ranges do not
overlap.

`MGET` of four keys on one shard costs 0.143 ms and one message per request;
`MGET` of four keys spread over four shards costs 0.183 ms and four messages.

**This is what explains the latency result above.** At eight shards, seven
requests in eight take the remote path. Predicted single-key latency is then
`0.079 + 7/8 x 0.064 = 0.135 ms`; measured, in a different experiment, **0.143
ms**. The two measurements corroborate each other, and together they say the
thing worth knowing: **shardkv's per-request latency is dominated by the
cross-shard hop, and it buys parallelism with it.**

### Memory

One million `SET`s over a million-key space, which lands about 632,000 distinct
keys (a million random draws from a million-key space covers about 63% of it) —
both servers received the same load and ended within 400 keys of each other.

| | baseline | after loading | growth |
|---|---|---|---|
| shardkv | 3,712 kB | 97,956 kB | **94,244 kB** |
| redis-server | 12,800 kB | 86,272 kB | **73,472 kB** |

shardkv uses **1.28x** the memory for the same data. The prediction said 1.5x to
3x, so this is better than expected — Redis has years of work on encoding small
values compactly, and a `std::unordered_map` of `std::string` was expected to
lose by more.

This is whole-process resident memory: the table, the allocator's retention and
fragmentation, per-thread stacks and buffers, and the process baseline. It is
**not** an isolated per-key overhead, which is why the baseline is reported
separately and why even the growth is not a clean per-key number.

### Predictions against outcomes

| # | prediction | outcome |
|---|---|---|
| 1 | one connection: within ~30% of Redis | **failed** — 81% slower, explained by the cross-shard hop |
| 2 | many connections: at least 2x Redis | **failed as specified** (0.67x), **held at 5.07x** once the generator was not the bottleneck |
| 3 | cross-shard `MGET` slower than same-shard | held — 0.183 vs 0.143 ms, 4 messages vs 1 |
| 4 | single-key penalty real, ranges disjoint | held — 0.064 ms, no overlap |
| 5 | memory 1.5x-3x Redis | **failed on the good side** — 1.28x |

`benchmarks/predictions.md` is the original text, committed before the runs.

### What these numbers do and do not support

They support: **shardkv trades per-request latency for parallelism, and the trade
pays once there is enough concurrent load to use it.** The cross-shard hop costs
0.064 ms and is taken by seven requests in eight at this shard count; with a load
generator able to keep eight loops busy, the result is five times Redis's
throughput on the same machine.

They do **not** support any claim about scaling with cores. That would need the
curve, and the curve is not measurable here.

Separately, and not from these figures at all, the sharding work established by
mechanism that **shared-nothing is implemented and free of data races**: keys
demonstrably spread across shards, the cross-shard path demonstrably taken (the
`cross_shard_requests` counter is read, not inferred), and ThreadSanitizer silent
over eighty thousand requests on fifty connections. **That is evidence the design
is the one described. It is not evidence that it scales well**, and the two are
easy to blur.

## Watching it under load

`scripts/soak.sh` keeps load on the server and samples RSS, the open descriptor
count and the `INFO` counters into a CSV. It is looking for a slope rather than
a number: flat curves say the buffers, the connections and the expiry sampler
all give back what they take. It prints no throughput figure, because a number
taken under an arbitrary background load is not a number.

```
docker run --rm -v "$PWD":/src -w /src shardkv-dev scripts/soak.sh
```

The default run is short so the script itself is exercisable; `SOAK_SECONDS=3600`
is the full hour.

## Known limitations

Beyond the deliberate omissions above, these are gaps of the current state
rather than of the design, and each closes in a later step:

- **A connection keeps a buffer as large as its largest burst.** The consumed
  prefix is compacted away, so a buffer no longer grows with the number of
  requests a connection has served -- but the allocation itself is kept and
  reused rather than returned. That is a choice, not an omission; the reasoning
  is in `docs/adr/`.
- **A slow client stalls, and is never disconnected.** Above a high watermark of
  pending output the connection stops being read from and TCP flow control
  pushes back on the sender; there is no output-buffer limit at which a client
  is closed. A client on a bad link waits rather than losing its replies.
- **Expired keys are freed on a sweep, not the instant they expire.** A key past
  its deadline is never *observable* -- every lookup enforces it -- but the
  memory comes back when the per-loop sampler next reaches it, so `DBSIZE` can
  briefly count keys that are already dead.
- **`CONFIG` is not implemented**, so `redis-benchmark` prints
  `WARNING: Could not fetch server CONFIG` before running normally.
- **A cross-shard `MSET` is not atomic.** Redis applies one indivisibly. Here
  the writes land on different threads at different moments, so a concurrent
  reader can see part of an `MSET` and not the rest. Single-key commands are
  unaffected.
- **Cross-shard commands cost an extra hop.** `MGET`, `MSET` and multi-key `DEL`
  are slower here than in single-threaded Redis when their keys span shards.
  That is the trade the architecture makes, not a defect.

## Design notes

- **Level-triggered epoll**, not edge-triggered: LT tolerates a missed read, ET
  turns one into a hung connection. `EPOLLOUT` is registered only when a `write`
  comes up short, and deregistered as soon as it drains.
- **The RESP parser is a pure function**: bytes in, either a parsed command plus
  a byte count, or "need more data", or a protocol error. It never touches a
  socket or the event loop, so it can be unit-tested by feeding it byte
  sequences — including split packets, coalesced packets, and malicious input.
- **`COMMAND` is implemented**, returning an empty array, because it costs
  nothing and clients may reasonably ask. It is often said that a recent
  `redis-cli` hangs without it; testing here did not reproduce that, and
  `docs/product.md` records what was actually observed.

## Licence

Not yet chosen.
