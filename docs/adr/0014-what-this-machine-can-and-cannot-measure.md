# What this machine can and cannot measure

summary: There is no scaling curve and no hardware-counter measurement in this repository, because every available machine is virtualised and its PMU is not reachable; a software-timer sampling profile is possible and is taken, under the limits recorded in 0017; throughput, latency, memory and the cross-shard penalty are measured against a same-machine Redis control group, and the difference is what is claimed rather than the absolute figures.

## Context

The obvious question about a project whose whole point is one event loop per
core is: how does throughput scale with cores? The plan had an answer — measure
at 1, 2, 4 and 8 cores and show the curve — and it is the sentence the project
most wants to be able to write.

It cannot be written, and the reason is the machine.

`perf`'s hardware counters are not exposed under virtualisation, and every
machine this has been developed on is virtualised. The first was
Docker-on-Windows, whose backend is WSL2, so entering WSL2 directly changed
nothing: it was the same hypervisor. The second is a Linux VM on an Apple
Silicon Mac, checked rather than assumed — the guest exposes no CPU event source
at all, and `perf list` returns no hardware event. Renting bare metal would solve it and
was declined.

Three consequences. The first two are about the hardware; the third is about
which claims survive.

- **The PMU is absent.** No cache-miss counts, no instructions-per-cycle, no
  `perf c2c` for false sharing. On an ARM host false sharing would additionally
  need the Statistical Profiling Extension, which is not there either.
- **A sampling profile is nevertheless possible.** The kernel can raise a
  periodic interrupt from a software timer that has nothing to do with the
  hardware counters, and that is enough for a hot-spot profile. It is taken, and
  what may be concluded from it is a decision of its own:
  `0017-the-profile-sees-user-space-only.md`. Hardware counters and sampling
  profiles are two capabilities, not one, and this record used to conflate them.
- **The scaling curve would be measurable and meaningless.** Numbers would come
  out. But hypervisor scheduling smears them: the cores a benchmark sees are not
  cores it owns, and a curve drawn through them describes the scheduler as much
  as the architecture. A number that cannot be attributed is worse than no
  number, because it looks like evidence. On the Apple Silicon machine there is a
  second, independent reason: its cores are not interchangeable. Some are
  performance cores and some are efficiency cores, the host decides which a shard
  lands on, and a design whose premise is one loop per core cannot be charted on
  a machine whose cores differ from one another.

## Decision

**No scaling curve, and no statement about scaling with cores.** Not in this
repository, not in the README, not anywhere the project's claims are repeated.
The sentence "throughput grows roughly linearly with cores" is unavailable.

**No hardware-counter measurement.** The part of the technical document's §8.3
that needs the PMU — `perf stat` for cache-miss, false-sharing analysis — is
unrunnable here and is marked so rather than quietly skipped.

**A sampling profile is taken.** The rest of §8.3 — where this program's own time
goes — is runnable with a software timer, and `benchmarks/profile.sh` runs it.
The limits that come with that instrument, and the claims it does and does not
support, are in `0017-the-profile-sees-user-space-only.md`.

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
