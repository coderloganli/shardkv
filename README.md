# shardkv

A concurrent in-memory key-value store in C++20, speaking the Redis wire protocol
(RESP2). The architecture is shared-nothing: one epoll event loop per core, the
keyspace partitioned across those cores, and no global lock anywhere on the
request path.

> **Status: work in progress.** The single-threaded server is written and its
> command set works against a real `redis-cli`. The part the project is actually
> about -- one event loop per core, the keyspace sharded across them -- is not
> built yet. There are no performance numbers here, and there will not be any
> until they have been measured on a named machine with a reproducible script.

## The idea

```
                    +--------------------------------------+
   TCP connection --▶| SO_REUSEPORT: the kernel assigns each |
                    | connection to one listening socket   |
                    +--------------------------------------+
                        |           |           |
                +-------▼---+ +-----▼-----+ +---▼-------+
                |  Loop 0   | |  Loop 1   | | Loop N-1  |  one thread per core,
                |  epoll    | |  epoll    | |  epoll    |  pinned by CPU affinity
                |  conns    | |  conns    | |  conns    |
                |  Shard 0  | |  Shard 1  | | Shard N-1 |  1/N of the keyspace
                +-----------+ +-----------+ +-----------+
                      ▲   |         ▲   |        ▲   |
                      |   +---------+---+--------+---+
                      +-------------+------------+
                     cross-shard requests: MPSC queue + eventfd
```

Each thread owns its epoll instance, its set of connections, and one hash table
shard that no other thread may touch. Threads share no mutable state; the only
channel between them is a message queue.

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

Sanitizer builds are selected with `-DSHARDKV_SANITIZER=address` or `=thread`.
The thread build additionally needs `--security-opt seccomp=unconfined` under
Docker; `docs/architecture.md` says why.

## Known limitations

Beyond the deliberate omissions above, these are gaps of the current state
rather than of the design, and each closes in a later step:

- **An expired key that is never accessed again is never freed.** Expiry is
  lazy; the background sampling that would reap untouched keys is not written
  yet, and `DBSIZE` counts such keys.
- **Buffers only grow.** A connection holds a read buffer as large as its
  largest burst, and a client that never reads its replies grows the write
  buffer without bound. Compaction and backpressure watermarks come with the
  resource-management work.
- **`CONFIG` is not implemented**, so `redis-benchmark` prints
  `WARNING: Could not fetch server CONFIG` before running normally.
- **Only one shard.** `--shards` exists and accepts 1; any other value is
  refused rather than silently ignored, because a benchmark labelled
  `--shards 8` that quietly ran on one loop would be worse than an error.

## Design notes

- **Level-triggered epoll**, not edge-triggered: LT tolerates a missed read, ET
  turns one into a hung connection. `EPOLLOUT` is registered only when a `write`
  comes up short, and deregistered as soon as it drains.
- **The RESP parser is a pure function**: bytes in, either a parsed command plus
  a byte count, or "need more data", or a protocol error. It never touches a
  socket or the event loop, so it can be unit-tested by feeding it byte
  sequences — including split packets, coalesced packets, and malicious input.
- **`COMMAND` must be implemented**, even if it only returns an empty array.
  Recent `redis-cli` sends `COMMAND DOCS` on connect and hangs without a reply.

## Licence

Not yet chosen.
