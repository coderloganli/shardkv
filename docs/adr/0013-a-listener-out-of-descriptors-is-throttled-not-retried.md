# A listener out of descriptors is throttled, not retried

summary: When `accept()` fails with `EMFILE` or `ENFILE` the loop drops `EPOLLIN` from its listener and re-arms it on the next timer tick, because under level-triggered epoll retrying immediately is an unbreakable spin.

## Context

The fault-injection work asked for one case: run the server with a very low
`ulimit -n` and check that a failing `accept()` is handled gracefully. Writing
that test found a defect rather than confirming a behaviour.

`Listener::accept()` returned an empty `UniqueFd` for every failure, mapping
`EMFILE` and `ENFILE` onto the same answer as `EAGAIN`, and its comment said the
caller "drops the event and carries on rather than dying, so that a descriptor
limit degrades service instead of ending it."

Under **level-triggered** epoll it does not carry on. `EAGAIN` means the backlog
is empty, so the listener stops being readable and the loop sleeps. `EMFILE`
means the backlog is *not* empty -- there is a connection waiting that could not
be given a descriptor. The listener stays readable, `epoll_wait` returns
immediately, `accept()` fails again, forever. A descriptor limit does not
degrade service on that loop; it burns a core and starves every connection
already on it.

This is the failure mode `docs/adr/0009-level-triggered-epoll-not-edge-triggered.md`
names as level-triggered's characteristic cost -- "anything registered but not
consumed reports itself ready on every turn" -- reached through an error path
rather than through a read.

## Decision

`Listener::accept()` takes a `bool*` out-parameter and sets it for `EMFILE` and
`ENFILE`, distinguishing "out of descriptors" from "nothing pending". The same
out-parameter shape the codebase already uses for `MpscQueue::pop(&busy)`.

On that signal the loop drops `EPOLLIN` from its listener and records the state.
The per-loop timer added for sampled expiry re-arms it on its next tick.
`LoopStats::accept_failures` counts the occurrences and `INFO` reports
`loopN_accept_failures`.

## Reasoning

**Stopping the notification is the only way to stop the spin.** The event is
level-triggered and the condition that raises it -- a connection in the backlog
-- is one the loop cannot clear while it has no descriptor to accept into. Either
the interest goes away for a while or the loop spins. There is no third answer
that keeps the listener registered.

**The waiting client is better served by a pause than by a refusal.** The common
cause of `EMFILE` is a transient burst, and a tick later there is very often a
descriptor free; the connection is then accepted normally and never learns
anything happened.

**What that claim is not.** Dropping `EPOLLIN` does not remove the listener from
its `SO_REUSEPORT` group, and how the kernel distributes connections among that
group is explicitly not something this design controls -- see the accept section
of `docs/architecture.md`. New connections may still be steered to a throttled
listener's backlog while another loop could have taken them. So the guarantee is
that a waiting connection is accepted once *this* loop has a descriptor again,
which is at least a tick and may be longer, not that it waits at most a tick.
The distinction matters less in practice than it reads: `RLIMIT_NOFILE` belongs
to the process, so when one loop is out of descriptors the others generally are
too, and the realistic alternative to waiting is not another loop's accept but a
refusal. Uneven or delayed distribution costs latency, not correctness, which is
the trade the accept path already makes.

**Re-arming on the existing timer rather than on a new one.** The loop now has a
periodic tick for sampled expiry
(`docs/adr/0012-a-timerfd-per-loop-drives-sampled-expiry.md`). Giving it a
second duty costs one branch. A dedicated timer, or a one-shot armed at the
moment of failure, would be a second lifecycle to get right for a case that is
not latency-sensitive.

**The alternative that Redis and nginx use, and why not here.** Both keep a
spare descriptor open: on `EMFILE` they close it, `accept()` the pending
connection, close it immediately so the client gets a clean refusal rather than
a hang, and reopen the spare. It gives a better answer to the client -- a
refusal now instead of a wait -- and it needs no timer. It also needs a reserved
descriptor per loop held for its whole life, a four-step dance in an error path
that is nearly never exercised, and correct behaviour when reopening the spare
*also* fails. With a tick already available, the throttle is a great deal less
machinery for an outcome that is arguably kinder. If a measurement ever shows
clients hanging on a full backlog rather than being refused, that is the reason
to revisit this.

**Counted, so the test can prove both halves.** Without `accept_failures` a test
can show the server survived a descriptor limit without showing it ever hit one
-- the same argument that put `peer_gone_writes` in `LoopStats`. It does a second
job here that CPU time would do worse: the spin executes one failed `accept()`
per turn of the loop, so under the defect the counter climbs by millions in a
fraction of a second, while the throttle produces one per tick. A test can bound
that number over a window and mean the same thing on a fast machine and a loaded
one, where "CPU stayed under some fraction of wall clock" is a guess about the
machine the test happened to land on.

## Consequences

`Listener::accept()`'s comment, which asserted that dropping the event was
enough, was wrong and is replaced rather than softened. It described the
intended behaviour of a design that level-triggered epoll does not permit, and
it is the kind of comment that stops the next reader from noticing.
