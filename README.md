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
> written down and committed before the runs. **Three of the five predictions
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
a thread and without taking a lock, and without allocating as long as its reply
fits inside a `std::string`'s own storage -- a boundary that is counted by a
test rather than asserted, and described under "Known limitations". A key that
does not land locally is forwarded to the owning loop as a message. That is the trade: synchronisation cost is paid per
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

## The test suite

`ctest` runs **233 entries**. 232 are GoogleTest cases; the 233rd, `bench_smoke`,
is a shell harness of 161 assertions that tests the benchmark scripts themselves —
the parsers that read `redis-benchmark` and `perf` output, the rule that
classifies a run as local or cross-shard, and each script's refusal to produce a
figure it cannot stand behind.

The GoogleTest cases sit roughly where the risk is: 32 on the store, 28 on the
RESP parser, 26 on expiry, 16 integration, 13 on dispatch, 12 each on sharding
and on `MGET`/`MSET` scatter/gather, 9 each on the reply slots, the MPSC queue
and the encoder, 8 counting heap allocations on the request path, 7 on
backpressure, and the rest on routing, buffers, ordering, fault injection,
connection lifetime and the cross-shard counter.

CI builds three ways and runs the whole suite in each: Release, ASan+UBSan, and
TSan. All three are green on x86_64 — the badge above is the live answer.

**Two suites do not run in every build, and both say so rather than being
quietly absent.** The allocation suite counts calls to `operator new`, which a
sanitizer runtime makes meaningless, so it is registered only where no sanitizer
is configured — and `tests/alloc_test.cc` refuses to compile under one, as a
second lock. `benchmarks/profile.sh` needs `perf` and a relaxed syscall filter;
where it cannot have them it refuses, and the smoke test asserts the refusal.
Neither case is skipped: both outcomes are definite and both are checked.

**On a machine that cannot name its own CPU.** An `aarch64` Linux guest on
Apple Silicon is told nothing about its processor — `/proc/cpuinfo` carries no
`model name` line at all, and `lscpu` answers `Vendor ID: Apple`,
`Model name: -`, `CPU part 0x000`. The environment recorder will not invent one,
so for a while nothing could be measured there at all.

It is not invented now either. The operator may **assert** it, the record marks
the value `(asserted)` where every reader will see it, the recorder warns while
doing it, and with neither a detected nor an asserted value it still refuses —
`docs/adr/0018-an-environment-field-may-be-asserted-never-guessed.md` sets out
why an assertion is a record and a guess is not.

## What it measures out at

Every figure here comes from `benchmarks/`, with its environment recorded beside
it and a script anyone can rerun. **Read the two caveats before the tables** —
they are what the figures mean.

**The machine.** AMD Ryzen 7 7800X3D, 8 cores, no SMT; Linux
6.6.87.2-microsoft-standard-WSL2 in a container on Docker Desktop; g++ 13.3.0,
`-O2` via `CMAKE_BUILD_TYPE=Release`; `redis-server` and `redis-benchmark` both
7.0.15; load generated on the same machine; threads not pinned. Eight shards.

**What is not here, and why.** There is **no scaling curve, and nothing measured
with a hardware counter** — no cache-miss figures, no instructions-per-cycle, no
false-sharing analysis. The machine is virtualised and its PMU is unreachable,
so a curve drawn on it would describe the hypervisor's scheduler as much as this
architecture, and the counters do not exist to be read. **No claim is made
anywhere that throughput scales linearly, or in any particular way, with
cores.** What replaces it is a control group under identical conditions —
`docs/adr/0014-what-this-machine-can-and-cannot-measure.md` has the reasoning.

A *sampling* profile needs no counters and is a separate thing: it is under
"Where the time goes" below, taken on a different machine, and it carries its
own account of what it can and cannot support.

### Throughput, and the instrument that was measuring itself

The technical document specifies `redis-benchmark -t get,set -n 1000000 -c 50`.
Run exactly that, and shardkv is **slower** than Redis:

| ops/s, `-c 50`, one generator thread | SET | GET |
|---|---|---|
| shardkv | 41,346 | 44,442 |
| redis-server | 63,625 | 69,823 |
| ratio | **0.65x** | **0.64x** |

`redis-benchmark` is single-threaded unless told otherwise. Fifty connections
through one client thread saturate that thread at somewhere around 45,000
requests a second — which is a fact about the benchmark, not about the server.
Redis, being itself single-threaded, is matched by it; eight loops are not.

Giving the generator eight threads changes which side is the bottleneck:

| ops/s, `-c 50`, eight generator threads | SET | GET |
|---|---|---|
| shardkv | 181,951 | 235,073 |
| redis-server | 51,886 | 49,324 |
| ratio | **3.51x** | **4.77x** |

Redis gets *slower* under this (from ~65,000 to ~50,000), because eight client
threads now compete with its one server thread for eight cores.

**Neither table is the true number, and the second does not rescue the first.**
The first measures the benchmark's ceiling; the second takes cores away from the
server on a machine that has only eight. The prediction was made against the
first, and against the first it failed — see the accounting below. What the
second establishes is *why*, and that is all it is offered as.

With pipelining, where one client thread can push over a million requests a
second and is no longer the constraint, shardkv is still behind:

| ops/s, `-c 50 -P 16`, one generator thread | SET | GET |
|---|---|---|
| shardkv | 978,474 | 984,252 |
| redis-server | 1,246,883 | 1,194,743 |
| ratio | **0.78x** | **0.82x** |

Sixteen commands per read means sixteen dispatches, and at eight shards about
fourteen of them cross a thread. Redis does the same sixteen in one thread with
no messages at all.

### Latency, one connection, no pipelining

| p50 | p95 | p99 | p99.9 |
|---|---|---|---|
| shardkv, GET | **0.143 ms** | 0.191 | 0.263 | 0.639 |
| redis-server, GET | **0.079 ms** | 0.103 | 0.143 | 0.239 |

shardkv is **81% slower per request**, where the prediction said about 30%.

### The cross-shard penalty

Each run is one connection against keys belonging to one shard, classified after
the fact by whether `cross_shard_requests` moved. 80 runs: 11 turned out local,
69 remote.

| | median | min | max | samples |
|---|---|---|---|---|
| keys on the connection's own loop | **0.079 ms** | 0.079 | 0.079 | 11 |
| keys on another loop | **0.143 ms** | 0.135 | 0.143 | 69 |

**The penalty is 0.064 ms per request**, and the two groups' ranges do not
overlap.

`MGET` of four keys on one shard costs 0.143 ms and one message per request;
spread over four shards it costs 0.167 ms and three.

**This is what explains the latency result.** At eight shards, seven requests in
eight go to another loop — so the *median* request is a remote one, and a
mixture's median is the median of whichever component holds the middle of it.
The remote group's median, measured in this experiment, is 0.143 ms. The
single-connection latency p50, measured in a different experiment, is **0.143
ms**. The same figure from two directions, and the local group's 0.079 ms is
exactly Redis's p50 — which is the shape the architecture predicts: **a local
hit costs what Redis costs, and shardkv pays 0.064 ms on the seven-eighths of
traffic that is not local.**

### Memory

One million `SET`s over a million-key space, which lands about 632,000 distinct
keys (a million random draws from a million-key space covers about 63% of it).
Both servers received the same load and ended within 1,100 keys of each other.

| | baseline | after loading | growth |
|---|---|---|---|
| shardkv | 3,712 kB | 97,804 kB | **94,092 kB** |
| redis-server | 12,800 kB | 86,424 kB | **73,624 kB** |

shardkv uses **1.28x** the memory for the same data — better than the 1.5x-3x
predicted.

This is whole-process resident memory: the table, the allocator's retention and
fragmentation, per-thread stacks and buffers, and the process baseline. It is
**not** an isolated per-key overhead, which is why the baseline is reported
separately and why even the growth is not a clean per-key number.

### Predictions against outcomes

Written in `benchmarks/predictions.md` and committed before any of this ran.
**Three of the five failed.**

| # | prediction | outcome |
|---|---|---|
| 1 | one connection: within ~30% of Redis | **failed** — 81% slower |
| 2 | many connections: at least 2x Redis | **failed** — 0.65x as specified. With a generator that is not itself the bottleneck it is 3.5x-4.8x, which explains the failure but does not undo it |
| 3 | cross-shard `MGET` slower than same-shard | held — 0.167 vs 0.143 ms, three messages against one |
| 4 | single-key penalty real, ranges disjoint | held — 0.064 ms, no overlap |
| 5 | memory 1.5x-3x Redis | **failed** — 1.28x, better than expected |

The two failures that matter are 1 and 2, and they are the same finding seen
twice: **the cross-shard hop costs 0.064 ms, seven requests in eight pay it, and
that is enough to lose to a single-threaded server whenever the load is not
concurrent enough to use eight loops.**

### What these numbers do and do not support

They support: at eight shards on this machine, **shardkv trades per-request
latency for parallelism.** A local hit costs about what Redis costs; a remote one
costs 0.064 ms more; and with a load generator able to keep eight loops busy, the
result was three to five times Redis's throughput on the same machine. Where the
load does not use that parallelism — one connection, or a single-threaded
generator, or heavy pipelining down one connection — shardkv is behind, and by
how much these tables say.

They do **not** support any claim about scaling with cores: that needs the curve,
and the curve is not measurable here. They are also a single shard count on a
single machine, so "three to five times" is one measurement and not a law.

Separately, and not from these figures at all, the sharding work established by
mechanism that **shared-nothing is implemented and free of data races**: keys
demonstrably spread across shards, the cross-shard path demonstrably taken (the
`cross_shard_requests` counter is read, not inferred), and ThreadSanitizer silent
over eighty thousand requests on fifty connections. **That is evidence the design
is the one described. It is not evidence that it scales well**, and the two are
easy to blur.

## Where the time goes

A separate experiment, on a **different machine and a different architecture**
from every figure above, and therefore not comparable with any of them. It is
here because it answers a question the tables above cannot: not how fast this
server is, but what it spends itself on.

**The machine.** Apple M5 Pro, 18 cores across two performance levels; an
`aarch64` Linux VM on macOS; g++ 13.3.0, `RelWithDebInfo`. Eight shards, load
generated on the same machine. The CPU model is **asserted, not detected** —
this guest is not told what it runs on — and the record says so.

**Read this before the table.** There is no PMU here, so the sampling is driven
by a software timer, and an unprivileged process may not sample the kernel. This
profile therefore sees **user-mode CPU time only**. In the run below the server
spent **13.1 seconds of user CPU and 31.6 seconds in the kernel**, so the table
covers a little under a third of what it actually did.

Where that other two thirds went, this profile does not say. The kernel was not
sampled at all, so nothing here attributes it to any particular call — the
figure is a total, and only a total. It is consistent with the picture the
visible third paints, and consistency is not attribution.
`docs/adr/0017-the-profile-sees-user-space-only.md` sets out the difference.

20 seconds, 13,478 samples, 11 million requests of load, 5.8% of samples on
addresses that did not resolve.

| share of user-mode time | symbol |
|---|---|
| 18.16% | `recv` |
| 11.38% | `__send` |
| 9.66% | `read` |
| 6.72% | `write` |
| 6.61% | `epoll_pwait` |
| 4.84% | `Loop::drainInbox` |
| 3.05% | `Loop::run` |
| 2.03% | `runOnShard` |
| 1.92% | `malloc` |
| 1.65% | `memcmp` |
| 1.47% | `dispatch` |

**What it supports.** The five system-call wrappers are better than half of the
visible time, and the store's lookup does not appear near the top at all. Add
that the kernel's share of total CPU is more than twice the part this table
covers, and the reading is that **this server's cost is dominated by the kernel
boundary rather than by anything it computes** — a reading, drawn from two
measurements that point the same way, not a single figure that proves it.

**What it does not support.** Any sentence of the form "X% of the time is spent
in Y". The denominator of that column is user-mode CPU, which is 29% of this
server's CPU and an unknown fraction of its wall-clock. Nor does a wrapper's
share say whether that call is cheap or expensive; the expensive part was never
sampled.

**What it changed.** Two things, and neither was the thing that looks obvious.

`docs/adr/0001-use-the-standard-library-hash-table-first.md` had deferred a
decision until there was a profile: write a hand-written open-addressing table,
or keep `std::unordered_map`. **The table stays.** It is not where the time
goes, and a faster one would compete for a slice of the smaller share. That is a
measured decision not to do a piece of work, and it took the profile to make it.

The change that was worth making was invisible in this table. `malloc` and
`cfree` are in it, and they say nothing about what the allocations were *for*.
An allocation counter — `tests/alloc_test.cc`, not a profiler — found the reply
queue taking a heap block every sixth command, on every connection, forever.
That was the whole of the gap between this server and its own published claim
that a common command allocates nothing. It is a ring buffer now
(`docs/adr/0019-the-reply-queue-is-a-ring-not-a-deque.md`), and `PING` and a
small local `GET` allocate nothing per request. **A profiler told us where not
to look; a different instrument found what to fix.**

**What is deliberately not attempted here.** No cache-miss counts, no
instructions-per-cycle, no false-sharing analysis, and no scaling curve. Those
need the hardware counters this machine does not expose, and the scaling curve
additionally needs cores that are interchangeable — which, on a chip with two
performance levels chosen by a host scheduler, these are not.

Reproduce it with:

```
docker run --rm --security-opt seccomp=unconfined -v "$PWD":/src -w /src shardkv-dev \
  bash -c 'cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build \
           && BENCH_CPU_MODEL="<what this machine is>" benchmarks/profile.sh'
```

The syscall filter has to be relaxed for `perf_event_open`, which is the same
flag the thread-sanitizer build needs for a different syscall. Without it the
script refuses and says so; it does not guess which of the three possible causes
it was.

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
rather than of the design. Nothing here is scheduled: the four steps this
project set out to do are done, so each of these is a known cost being paid
knowingly, not work already queued up:

- **A reply larger than a `std::string`'s own storage costs two allocations.**
  One to hold its bytes and one to give them back. The shard encodes into a
  string before the slot hands it to the write buffer; encoding straight into
  that buffer would remove both, and would change what dispatch is — it would
  write replies rather than return them — reaching into the ordered-slot design.
  Counted by `tests/alloc_test.cc` rather than estimated. Small replies, and
  `PING`, allocate nothing.
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

MIT — see `LICENSE`. The one vendored dependency, `third_party/xxhash`, is Yann
Collet's xxHash under the BSD 2-Clause licence, whose text is kept beside it.
