# Predictions

Written before the measurements were run, and committed before any results
existed. Check the git history: this file's commit precedes the results commit.
That is the only thing that makes these predictions rather than descriptions —
no test can enforce it, because a test cannot tell when a sentence was thought
of.

Three of these come from the technical document's §8.2, which stated them before
the server was written. Two are new, for the measurements §8.2 did not cover.

**If a measurement disagrees with its prediction, that is the most valuable
output this step has.** The instinct is to adjust the prediction; the job is to
find out why and write the finding into the README.

## What "the difference" means here, and why it is the claim

Everything below is a comparison against `redis-server` on the same machine under
the same load. The machine is virtualised and the load generator competes with
the server for the same eight cores, so the absolute figures are depressed and
noisy. Both sides run under exactly that, so the *difference* survives what the
absolutes do not. See
`docs/adr/0014-what-this-machine-can-and-cannot-measure.md`.

## 1. One connection, no pipelining: shardkv is close to Redis, or a little slower

Redis's single-threaded path is short and has had a decade of attention. There is
no parallelism to exploit at one connection, so shardkv's architecture buys
nothing here and its extra indirection — the router, the reply slots — costs
something.

**Predicted:** shardkv's p50 within about 30% of Redis's, on either side, and
plausibly worse. A large gap in *either* direction would be a surprise worth
chasing: much slower means the single-key path has a cost that should not be
there, and much faster means the comparison is not measuring what it claims.

## 2. Many connections across cores: shardkv is well above Redis

This is the project's entire premise. Redis serves from one thread and cannot use
more than one core; shardkv runs eight loops on eight cores.

**Predicted:** shardkv's throughput at `-c 50` is at least twice Redis's.

**And the caveat that makes this honest:** the benchmark also wants cores, so
neither side gets the machine to itself, and the effect is worse for the side
that could have used more of it. If this prediction fails, contention is the
first suspect and the finding to write down is which of the two the machine
starved.

**This is not a scaling result and must not be reported as one.** It is one
point of comparison at one shard count. The curve is not measurable here and no
statement about scaling with cores may be made from it.

## 3. Cross-shard multi-key commands: shardkv is slower than Redis

`MGET` over keys on four shards costs up to four messages between threads, where
Redis reads four keys in one thread with no messages at all. §3.2 of the
technical document conceded this before any of it was built, and the README
already states it.

**Predicted:** `MGET` over four keys spread across four shards is measurably
slower than `MGET` over four keys on one shard, and the counter confirms the
spread version sent more messages. A *smaller* difference than expected would be
interesting: it would mean the message path is cheaper than the design assumed.

## 4. The single-key cross-shard penalty is small but real

An extra hop is a queue push, an eventfd write that is usually elided, a wake and
a queue pop. Micro-seconds, against a request that already costs tens of
micro-seconds on this machine.

**Predicted:** the remote group's median latency exceeds the local group's, and
the two groups' ranges do not overlap. The overlap is the part I am least sure
of: the local group gets about one sample per sweep, and on a noisy virtual
machine ten samples may not separate. **If the ranges overlap, the honest result
is that this machine could not resolve the penalty** — not a penalty of zero, and
not a number quoted anyway.

## 5. Memory after a million keys: shardkv uses more than Redis

Redis has had years of work on encoding small values compactly and shardkv stores
a `std::string` per key and per value in a `std::unordered_map`, with a table and
a thread and buffers per shard.

**Predicted:** shardkv's resident growth is larger than Redis's, by somewhere
between 1.5x and 3x.

**What this figure is not:** whole-process RSS includes the allocator's
retention, the per-thread stacks and buffers, and the process baseline. Eight
threads on one side against one on the other. The baseline is recorded separately
so the growth can be looked at on its own, and even the growth is not a clean
per-key number.
