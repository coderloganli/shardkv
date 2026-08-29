# Backpressure is two watermarks, and never a disconnect

summary: A connection stops being read from once its write buffer reaches a high watermark and resumes below a low one; no client is ever closed for being slow, and there is no output-buffer kill limit.

## Context

A client that pipelines commands and never reads its replies grows the
connection's write buffer without bound. Nothing in the server stopped it: the
loop kept accepting readable events, kept parsing commands, and kept appending
replies that the socket would not take.

The mechanism to push back is not in question -- it is the one every server of
this shape uses. Stop reading from the socket, let the kernel receive buffer
fill, and let TCP flow control stall the sender. What had to be decided is how
resumption is triggered, and whether stalling is the whole answer or whether a
client can also be too slow to keep.

Redis answers the second question with `client-output-buffer-limit`: past a hard
threshold the client is closed. That is a real option and it is the obvious one
to copy.

## Decision

**Two watermarks.** At or above 1 MiB of pending output, the connection stops
being read from: `wantsRead()` turns false and the loop drops `EPOLLIN`. At or
below 256 KiB it resumes. The state is one `bool` on the connection.

**No hard limit, and no disconnect.** There is no threshold at which a slow
client is closed. The watermarks are the entire policy.

## Reasoning

**One threshold oscillates; two do not.** With a single line, a connection that
drains one byte below it is read from again, immediately climbs back over, and
pauses again -- an `EPOLL_CTL_MOD` per byte at the boundary. The gap between the
two is what turns that into one pause and one resume. 256 KiB is a quarter of
1 MiB: enough that a resumed connection has real work to do before it can pause
again, small enough that the socket does not sit idle waiting to be refilled.

**Stalling already bounds the memory, so a kill limit buys nothing.** This is
the argument that decided the second question. Once reading stops, no further
command is parsed, so the write buffer cannot grow past the replies to what was
already read -- one read chunk's worth of commands, plus the reply in flight.
The growth was unbounded because parsing was unbounded; bounding parsing bounds
the buffer. A kill threshold would be a second mechanism defending a property
the first one already holds.

**And a kill limit is a policy that can be wrong.** "Slow" and "malicious" look
identical from inside the server: a client on a bad link and a client attacking
the process both read late. Stalling treats them the same and harms neither --
the slow client waits, the attacker occupies one bounded buffer and stalls only
itself. Closing treats them the same too, and harms both. Given that the memory
is already bounded, the version that never drops a legitimate client's replies
is the better one.

**What is measured is the residual, not the peak.** The check runs at the end of
`flush()`, which is not the only place the write buffer changes -- `takeReadyPrefix()`
appends to it first -- but is where every path that changes it comes to rest.
That is deliberate rather than incidental: a burst of replies the socket swallowed
whole leaves nothing pending and is no reason to stop reading, so pausing on the
transient peak before the send would throttle connections that are keeping up.
The quantity backpressure is about is what the client has not taken yet.

**Reusing the existing interest mechanism rather than adding one.**
`Loop::updateInterest()` already computes epoll interest from `wantsRead()` and
`wantsWrite()` and issues `EPOLL_CTL_MOD` only when the answer changes. It was
built for the terminal-slot case, where a connection after `QUIT` must stop
being read from or the level-triggered listener spins. Backpressure is a second
reason for the same answer, so it is a second term in `wantsRead()` and nothing
else.

**The invariant that makes this safe, stated narrowly.** It is tempting to say
that a connection registered for no epoll events is wedged, but that is not true
of this codebase and the imprecise version would be the wrong thing to defend:
after `QUIT` or a protocol error a connection has `wantsRead()` false and may
have an empty write buffer while cross-shard replies are still outstanding, and
it is perfectly fine -- those replies come back on the loop's eventfd and
`drainInbox()` reaches the connection by identifier rather than through epoll.
Something else is coming for it.

A **paused** connection has no such second mover. Nothing else in the process is
waiting to act on it; the only event that can resume it is its own socket
draining, and the only thing that reports that is `EPOLLOUT`. So the invariant is
specific: **while a connection is paused, it must want to write.** It holds
because pausing requires at least 1 MiB pending and resuming happens at 256 KiB,
so there are always bytes left. It is pinned by a test rather than by this
paragraph, because it is the one way this feature can hang a client forever.

**`read_pauses` is counted and reported.** `INFO` gains `loopN_read_pauses`, for
the same reason `peer_gone_writes` exists: without it, a test can show that the
server survived a slow client without showing that backpressure was ever
engaged, and a test that cannot fail proves nothing.

## Alternatives

**A hard output-buffer limit that closes the connection.** Rejected above: the
watermarks already bound the memory, so the limit adds only the ability to be
wrong about a slow but honest client. If a future case appears where a single
reply is itself too large to hold -- which is a different problem, since it
cannot be fixed by reading less -- that is when to revisit this.

**Shrinking the socket's `SO_RCVBUF` instead of deregistering `EPOLLIN`.**
Pushes back through the same TCP mechanism but leaves the server reading, so the
buffer keeps growing; it slows the sender rather than stopping the parse.

**Dropping replies for a client that cannot keep up.** RESP matches replies to
commands by position alone, so a dropped reply desynchronises the stream for
every command after it. Not an option in this protocol.
