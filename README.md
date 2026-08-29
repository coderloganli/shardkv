# shardkv

A concurrent in-memory key-value store in C++20, speaking the Redis wire protocol
(RESP2). The architecture is shared-nothing: one epoll event loop per core, the
keyspace partitioned across those cores, and no global lock anywhere on the
request path.

> **Status: work in progress.** The server runs N event loops on N threads with
> the keyspace partitioned across them, answers the v1 command set correctly at
> any shard count, and holds its resources: buffers compact, a client that will
> not read its replies is stopped being read from, and expired keys nobody
> touches again are reaped in the background. What is left is measurement. There
> are no performance numbers here, and there will not be any until they have
> been measured on a named machine with a reproducible script.

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

There is a `Dockerfile` carrying the toolchain and `redis-tools`, which is how
this is developed and how CI runs:

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
