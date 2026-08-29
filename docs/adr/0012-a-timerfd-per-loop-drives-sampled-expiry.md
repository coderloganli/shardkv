# A timerfd per loop drives sampled expiry

summary: Each loop owns a timerfd in its own epoll set, ticking every 100 ms; the tick runs bounded sampling passes against that loop's own shard, and `Shard` exposes the single pass so it is testable against a manual clock with no sleeping.

## Context

Expiry landed in two halves, as `docs/architecture.md` said it would. The lazy
half is done: `Shard::lookup` checks the deadline and erases in place, so no
command can observe an expired value. The gap is a key that expires and is never
looked at again -- nothing frees it, and `DBSIZE` counts it. That gap is pinned
by a test, deliberately, so that closing it could not happen quietly.

Closing it needs two things that are easy to run together and should not be: a
periodic tick, and a bounded amount of reaping per tick.

## Decision

**`Shard::sampleExpired(limit)` is one bounded pass** and returns what it did --
how many keys it visited and how many it erased. A persistent cursor of
`{bucket, offset}` walks the table, wrapping the bucket index modulo
`bucket_count()`, visiting at most `limit` keys and stopping after a full lap if
that comes first. Expired keys are
collected during the walk and erased after it.

**A `timerfd` per loop drives it.** `CLOCK_MONOTONIC`, `TFD_NONBLOCK |
TFD_CLOEXEC`, 100 ms period, registered in that loop's own epoll set. On a tick
the loop runs passes of 20 keys against its own shard, continuing while a pass
erased more than a quarter of what it visited, up to 8 passes.

**`Shard::size()` still does not sweep.** `DBSIZE` reports the table as it
stands. What changed is that the table converges, because something else reaps
it.

## Reasoning

**Splitting the pass from the tick is what makes this testable.** The pass takes
its time from the shard's `Clock`, which is the seam `store/clock.h` was built
with. A `ManualClock` therefore drives the whole reaping behaviour -- bounds,
cursor advance, rehash, the lot -- with no sleeping and no timing assumptions. A
test that sleeps is slow when it passes and flaky when it does not, and reaping
is precisely the kind of behaviour that would otherwise be tested by waiting and
hoping. One integration case, and only one, proves the timer is actually wired
to the pass.

**A timerfd rather than a timeout on `epoll_wait`.** A timeout must be
recomputed from the time actually elapsed on every turn of the loop, and a loop
woken frequently by traffic keeps restarting its own deadline -- a busy loop
would sample late or never, which is the case where sampling matters most. The
arithmetic is right the day it is written and wrong after the third edit. A
timerfd makes the tick a descriptor, `run()` already dispatches on
`events[i].data.fd`, and `epoll_wait` keeps its infinite timeout.

**A cursor rather than random probing.** Redis samples randomly; an
`unordered_map` offers no way to pick a uniformly random element without walking
to it. The cursor is not a workaround for that but a sweep, which is a different
and in some ways better property: **a lap with no intervening rehash visits every
key**, so no expired key can survive behind a bucket the sampler keeps missing.
The claim stops there deliberately. A rehash moves elements between buckets, so a
lap that straddles one can visit a key twice or not at all; rehashing is caused
by insertion, the sweep carries on, and the next lap covers the table again.
Asserting a per-lap guarantee that survives rehashing would be asserting
something the container does not offer.

**The cursor needs an offset, not just a bucket index.** A bucket index alone
cannot both bound a pass at `limit` visits and make progress: a bucket holding
more entries than `limit` would be restarted from its front every time, and
everything past the limit would never be reached at all. With the offset, a pass
that runs out of budget mid-bucket resumes exactly there. This is the kind of
thing that looks like an implementation detail and is actually the difference
between a sampler that terminates and one that does not, so a test pins it.

**Collect then erase, and be exact about what is collected.**
`unordered_map::erase` takes a `const_iterator`; a bucket walk yields
`local_iterator`s, which it will not accept, and erasing mid-walk would
invalidate the walk in any case. So the walk collects `const std::string*`, the
address of each doomed element's key, and afterwards erases each as
`map_.erase(map_.find(*key))`. Erasing an element invalidates references and
pointers to *that* element alone, so the pointers still in the list remain valid
as the list is worked through. The price is one extra hash and lookup per expired
key -- paid only for keys that are being destroyed anyway.

**The numbers.** 20 keys a pass and at most 8 passes bounds a tick at 160 keys
examined. That bound is the point -- a tick does an amount of work that does not
depend on how large the shard is -- and no claim is made here about what it costs
in latency, because no such measurement has been taken; this project does not put
performance statements in writing before they have been produced by a script on a
named machine. The quarter-erased rule is what lets a shard that has just had a
million keys expire reclaim them quickly rather than at 200 keys a second, while
a shard with nothing to find stops after one pass. 100 ms rather than 10 ms
because an idle server should not wake ten times more often than it needs to; an
idle tick is one `read()` and one walk that finds nothing.

**Each loop reaps only its own shard, and this is not a place a thread could
escape.** The tick fires on the loop's own thread and touches the shard that
thread already exclusively owns, so sampling adds no cross-thread reachability
at all. That is worth stating because a "background cleaner" is the shape of
thing that usually arrives as a separate thread with a lock, and here it must
not.

## Consequences

`tests/expiry_test.cc`'s `DbsizeCountsExpiredKeyNeverLookedUp` pinned the gap
this closes and fails as soon as this lands. It is rewritten to assert the new
behaviour -- DBSIZE falls once a pass has run -- and neither deleted nor
accommodated by weakening the sampling. That was written into the test's own
comment when it was created, and this is the record of it being honoured.
