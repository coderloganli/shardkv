# Level-triggered epoll, not edge-triggered

summary: The event loops use level-triggered epoll; edge-triggered is a roadmap item with a measurement attached, not a v1 choice.

## Context

`epoll` offers two notification modes, and a server built on it has to pick one
before anything else is written, because the reading and writing code differs.

**Level-triggered.** The kernel reports a descriptor as ready for as long as it
*is* ready. Read some of what is available and epoll will tell you again.

**Edge-triggered.** The kernel reports readiness once per transition. Having been
told, you must read until `read()` returns `EAGAIN`, because there will be no
second notification for data you left behind.

This was recorded as prose in `docs/architecture.md` and is written down properly
here, alongside the other decisions that shape the request path.

## Decision

Level-triggered, on both the read and the write path. `EPOLLOUT` is registered
only while a write is outstanding — under level-triggered semantics a
permanently-registered writable socket would report itself writable on every
turn of the loop and spin it.

## Reasoning

**The failure modes are not symmetric, and that decides it.** Getting
level-triggered wrong means an extra wakeup: the loop is told again about data
it already knew about, and does a little redundant work. Getting edge-triggered
wrong means a connection that silently stops making progress — bytes sat in the
kernel buffer, no further notification is coming, and the client waits forever.
The first is a small inefficiency; the second is a hang that reproduces under
load, on someone else's machine, once a week.

**The efficiency that edge-triggered buys is smaller here than it looks.** Its
advantage is fewer events per unit of data, which matters most when each
notification yields little. This server is pipelined: a single read commonly
carries several commands, so the events-per-command ratio is already low and ET
would improve a number that is not the bottleneck.

**The obligation ET imposes is easy to state and easy to breach.** "Always read
until `EAGAIN`" is one sentence, and every future read path — a new command
type, a resumed partial read, an error branch that returns early — has to honour
it. A rule that must be re-obeyed at every call site is a rule that will
eventually be missed, and its breach is invisible until the hang.

**Deferred rather than rejected.** If the measurement step shows event overhead
is material, the read path can move to ET with a read-until-`EAGAIN` loop, and
the difference measured against the level-triggered baseline. That is the same
shape as the other deferrals here — see
`docs/adr/0001-use-the-standard-library-hash-table-first.md` — and it has the
same reason behind it: the change is worth making only if a profile asks for it,
and worth reporting only if there is a baseline to compare against.
