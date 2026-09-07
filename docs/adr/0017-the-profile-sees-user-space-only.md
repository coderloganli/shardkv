# The profile sees user space only

summary: Sampling profiles are taken with a software timer that counts only user-mode CPU time, because this machine exposes no PMU and an unprivileged process may not sample the kernel; in a run that passes the recording script's quality gates the ordering of user-space hot spots may be relied on, while any statement of the form "X% of the time is spent in Y" may not, and the profile is published with its denominator beside it.

## Context

`docs/adr/0014-what-this-machine-can-and-cannot-measure.md` records that this
machine has no reachable PMU. That is still true, and it rules out cache-miss
counts, instructions-per-cycle and false-sharing analysis.

It does not rule out a profile. A sampling profiler needs a periodic interrupt,
and the kernel can raise one from a software timer that has nothing to do with
the hardware counters. Measured rather than assumed: with the container's
default syscall filter relaxed, recording against the running server produces a
profile whose symbols resolve on both sides of the library boundary.

Two limits come with that timer, and both change what may be said.

**The timer counts only user-mode CPU time.** An unprivileged process may not
sample the kernel, so the recording is forced to exclude it. A thread that has
entered the kernel to perform a system call accrues nothing while it is in
there; it produces no samples at all.

**The denominator is small, and it is not wall-clock.** In a ten-second window
against a saturating load, the server accrued under two seconds of user-mode CPU
across all of its threads. Whatever the rest of that time was — kernel work,
or blocking — this instrument cannot see it and cannot divide it.

## Decision

**Sampling profiles are recorded, and hardware counters are not.** These are two
different capabilities and the project states them separately. Having one is not
having the other.

**What may be claimed from an accepted profile: the ordering of user-space hot
spots.** Which of this program's own functions costs more than which, and
therefore where to look first. That is what a profile is for here.

**Accepted is not the same as recorded**, and the difference is enforced by the
script rather than judged afterwards. A profile is accepted only if it cleared
every gate `benchmarks/profile.sh` applies: enough samples to be more than
noise, load that covered the whole recording window, and few enough unresolved
addresses that the names mean something. A recording that failed any of them is
refused outright rather than published with a caveat — an ordering read off too
few samples is not a weaker result, it is a different one.

**What may not be claimed: any share of total time.** "X% of the time is spent
in Y" is unavailable, because the denominator of these percentages is user-mode
CPU time, not wall-clock and not total CPU. A syscall wrapper that appears high
in the list is not evidence that the syscall is cheap or expensive; the kernel
side of it was never sampled.

**The denominator is published with the figures.** Every profile records its
sample count, its user-mode CPU time, and — from the process itself rather than
from the profiler — its kernel-mode CPU time. The last of those does not say
which kernel function is hot. It sizes the part of the picture that is missing,
which is the least a reader needs in order to know how much of the whole this
table covers.

## Reasoning

**A profile with a hidden denominator is the failure this project keeps
avoiding.** The hot-spot table looks exactly like the familiar one, and a reader
will supply the usual denominator — wall-clock — unless told otherwise. The
number is not wrong; the sentence the reader forms from it is. Printing the
denominator costs one line and removes the whole failure.

**The ordering survives the limitation and the share does not.** Both are
measured against the same clock, so comparing two user-space functions to each
other is sound. Comparing either to the whole is not, because the whole is
partly unmeasured. Keeping the first and refusing the second is what the
instrument actually supports.

**Understating rather than overstating.** Because the kernel side is invisible,
the true cost of the system-call path is larger than this profile shows, not
smaller. A reader who takes the table at face value will underestimate it. That
is the safer direction for an error to run, and it is stated rather than left to
be worked out.

## Consequences

The profile answers "where should I look first" and refuses "how much of the
total is this". Optimisation work in this repository is directed by the first
question, and any before-and-after claim is made by re-measuring throughput or
latency — which are wall-clock measurements against a control group — rather
than by comparing two profiles.

If bare metal ever becomes available, the hardware counters this record works
around become readable, and the ordering established here is what says which
function is worth a counter.
