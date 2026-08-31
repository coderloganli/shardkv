# What this machine can and cannot measure

summary: There is no scaling curve and no profile in this repository, because the only available machine is virtualised and its PMU is not reachable; throughput, latency, memory and the cross-shard penalty are measured against a same-machine Redis control group, and the difference is what is claimed rather than the absolute figures.

## Context

The obvious question about a project whose whole point is one event loop per
core is: how does throughput scale with cores? The plan had an answer — measure
at 1, 2, 4 and 8 cores and show the curve — and it is the sentence the project
most wants to be able to write.

It cannot be written, and the reason is the machine.

`perf`'s hardware counters are not exposed under virtualisation. The only
machine available is Docker-on-Windows, whose backend is WSL2, so entering WSL2
directly changes nothing: it is the same hypervisor. Renting bare metal would
solve it and was declined.

Two consequences, and it is worth separating them because only one is about
tooling:

- **The PMU is simply absent.** No `perf record`, no flame graph, no `perf stat`
  for cache misses or false sharing. Confirmed rather than assumed: `perf` is not
  installed in the image, and would not have working counters if it were.
- **The scaling curve would be measurable and meaningless.** Numbers would come
  out. But hypervisor scheduling smears them: the cores a benchmark sees are not
  cores it owns, and a curve drawn through them describes the scheduler as much
  as the architecture. A number that cannot be attributed is worse than no
  number, because it looks like evidence.

## Decision

**No scaling curve, and no statement about scaling with cores.** Not in this
repository, not in the README, not anywhere the project's claims are repeated.
The sentence "throughput grows roughly linearly with cores" is unavailable.

**No profiling section.** The technical document's §8.3 — `perf record`, flame
graphs, `perf stat` for cache-miss and false-sharing — is unrunnable here and is
marked so rather than quietly skipped.

**What is measured instead**, all of it labelled as taken on a virtual machine
with neighbour noise: throughput and pipelined throughput, the latency
distribution, resident memory after a million keys, and the cross-shard penalty.

**Every one of them against a same-machine `redis-server` control group, and the
difference is what is claimed.** Both sides run under the same hypervisor, the
same scheduler and the same neighbours, so what the noise does to one it does to
the other. The absolute figures are recorded and are honest about being
depressed; the differences are what survive.

**The architecture's central claim rests on mechanism evidence, not on a curve.**
Step 2 established three things directly: keys really are distributed across
shards, the cross-shard path really is executed — the `cross_shard_requests`
counter is not inferred, it is read — and ThreadSanitizer reports nothing over
eighty thousand requests on fifty connections. Together those say
**shared-nothing is implemented and free of data races**. They do **not** say it
scales well. The README states that distinction in those terms, because a reader
who is not told will assume the stronger claim.

## Reasoning

**The rule does not bend for the thing it costs most.** "No unmeasured
performance claims" is one of this project's stated principles, and the scaling
sentence is exactly the case that tests whether a principle is real. Not measured
is not measured.

**A control group is what is left when absolutes are unreliable, and it is worth
more than it looks.** "shardkv is faster than Redis at fifty connections on this
machine, and slower at one" is a claim about two programs under identical
conditions. It survives the noise that destroys "shardkv does N operations per
second".

**Mechanism evidence answers a different question, well.** It cannot say how
fast the design is. It can say the design is the one described, which is the
claim a reader of the source most needs, and it is checkable rather than
believed.

## Consequences

Someone will ask where the scaling curve is. The answer is here, and it is a
better answer than a curve drawn on a hypervisor would have been: the machine
could not support the claim, so the claim was not made.

If bare metal ever becomes available, this record is what says what to run and
what would then become sayable. Nothing in `benchmarks/` assumes the machine it
ran on — the scripts take their environment from the machine and record it — so
the same scripts produce the missing figures there.
