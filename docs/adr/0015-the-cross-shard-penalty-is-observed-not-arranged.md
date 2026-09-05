# The cross-shard penalty is observed, not arranged

summary: A benchmark run cannot be made all-local or all-remote, because the kernel decides which loop accepts a connection; each run is instead classified after the fact by whether `cross_shard_requests` moved, and the penalty is the difference between the two groups.

## Context

The cross-shard penalty is the price this architecture charges: a key belonging
to another loop is forwarded as a message instead of being read in place. The
README already claims that price exists. Measuring it means comparing a load
that never crosses a thread against one that always does.

Neither can be arranged with the tools here.

- `redis-benchmark` generates `key:__rand_int__` and has no way to choose keys by
  shard. That half is solvable: a small tool reusing the server's own
  `shardForKey` prints keys for a chosen shard.
- The other half is not. **Which loop accepts a connection is the kernel's
  choice.** `SO_REUSEPORT` distributes connections by a rule `socket(7)` does not
  document and a BPF program can redefine, and `docs/architecture.md` says
  plainly that this design neither controls nor assumes it. A benchmark cannot
  ask for a connection on loop 3.

So "all local" is not something a script can request.

## Decision

It is something a script can **recognise**. `INFO` reports
`loopN_cross_shard_requests`, a counter that exists so a local hit can be told
from a round trip. Each run is bracketed:

1. Read the summed counter.
2. Run `redis-benchmark -c 1 -k 1 -n N GET <key of shard S>`. One connection,
   kept for the whole run, so its loop is fixed for the duration.
3. Read the counter again.

A delta near zero means the connection's loop happened to own shard S — that run
was all-local. A delta near N means every request crossed a thread — that run was
all-remote. The script sweeps every shard, several times over, and compares the
median latency of the runs that turned out local against those that turned out
remote.

**It reports the sample count of each group, and refuses to report a penalty when
either group has fewer than three runs**, saying so instead.

The multi-key path is measured separately and needs no locality: `MGET` of four
keys on one shard against `MGET` of four keys on four shards. One remote group
costs one message and four cost up to four, and the counter confirms how many
were actually sent.

## Reasoning

**Reading the mechanism beats predicting it.** The alternative is to assume a
connection's loop from some property of the connection, which is exactly the
assumption `docs/architecture.md` refuses to make about `SO_REUSEPORT` and which
an earlier version of that document got wrong. A measurement resting on it would
be wrong in the same way, and silently.

**The counter was built for this.** `cross_shard_requests` was added so a test
could tell a cross-shard round trip from a lucky local hit — without it, a test
that stores and reads back a "remote" key proves nothing about the path it took.
This is the same question asked at benchmark scale, so it is the same instrument.

**And the instrument is inside the experiment.** `INFO` is itself a cross-shard
command: reading the counter fans out to the other loops and adds about
`shards - 1` to the number being read, which `docs/architecture.md` already
records as a property of the counter rather than a flaw in it. Both bracket
readings pay that cost. At tens of thousands of requests against single digits of
overhead it is immaterial — but only because the threshold is set with it in
mind. Classification splits at half the request count, not at zero, so a run
cannot be misread by any amount of instrument cost.

**One connection, not fifty.** With fifty connections spread over eight loops,
every run is a mixture and there is no clean pair to compare. At one connection
the run is entirely one thing or entirely the other, and the counter says which.
Throughput at one connection is not interesting; **latency is, and latency is
what a penalty means** — the extra hop shows up as time per request, not as a
ceiling on requests per second.

**The sample counts are reported because they are the weak part.** Each run is
local with probability 1/N. A sweep of N runs yields about one, so several sweeps
are needed and the local group stays the small one. Publishing a difference
computed from one or two runs would be publishing noise with a label on it; the
refusal threshold is what stops that happening quietly on a machine where the
kernel's distribution happens to be lopsided.

## Alternatives

**Compare `--shards 1` against `--shards 8`.** Needs no new tooling, and
confounds two variables: the eight-shard run differs in parallelism as well as in
locality, so the difference cannot be called a cross-shard penalty.

**Only measure `MGET`.** Works with fixed keys and no locality problem, but the
multi-key path is not where most of the cost lives: at eight shards roughly seven
in eight single-key requests take the remote path, and that is the case the
architecture trades on.

**Pin connections to loops with a BPF program.** `SO_REUSEPORT` allows it, and it
would make the arrangement exact. It also means writing and loading BPF to
measure a server that otherwise needs no privileges, in order to test a
configuration nobody runs.
