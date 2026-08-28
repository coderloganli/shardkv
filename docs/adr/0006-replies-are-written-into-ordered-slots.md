# Replies are written into ordered slots

summary: Each parsed command reserves a slot in a per-connection queue at parse time; a cross-shard reply fills its own slot whenever it returns, and the connection flushes only the longest filled prefix.

## Context

RESP has no request identifiers. A client matches replies to commands purely by
order: the first reply belongs to the first command, and a pipelining client
sends many commands before reading any of them. So replies must leave the socket
in the order the commands arrived, without exception.

That was free while every command was answered on the spot. Once a key can
belong to another thread, it stops being free: a command that goes cross-shard
finishes after a message round trip, while the commands behind it in the same
pipeline may be local and finish immediately.

So the connection has to hold a finished reply back until everything ahead of it
is done.

## Decision

Each connection owns an ordered queue of reply slots. Parsing a command pushes a
slot. A local command fills its slot before parsing continues; a cross-shard
command leaves its slot empty and the reply fills it when it arrives.

After any completion, the connection appends the longest run of filled slots
from the front of the queue to its write buffer and pops them. A gap stops the
flush; everything behind the gap waits.

Parsing does not stop at a cross-shard command. The connection keeps reading and
keeps dispatching.

## Reasoning

**The alternative was to stop reading that connection until the in-flight
command completes**, which needs no slots at all and is markedly less code.

It was rejected on what it would do to the numbers. With N shards and
well-distributed keys, roughly (N-1)/N of commands are cross-shard — at eight
shards, seven in eight. Halting the connection on each of those turns a
pipelined run into a sequence of round trips, so `redis-benchmark -P 16` would
measure latency, not throughput.

That matters more here than it would elsewhere, because pipelined throughput is
one of the figures this project reports. A design that quietly serialised
pipelines would produce a number that understates the architecture, and then the
number would be presented as if it described it. Being slow is survivable;
publishing a measurement of the wrong thing is not.

**A gap blocks only the connection it is on.** Slots are per-connection, so a
loop with one connection waiting on a cross-shard reply keeps serving every
other connection it owns. The head-of-line blocking is confined to the client
that caused it, which is also exactly what that client asked for by pipelining.

**Slots make the ordering invariant checkable in one place.** "Flush the longest
filled prefix" is a single function, and it is the only place that decides what
goes onto the wire. The alternative — each command path minding the ordering
itself — spreads that obligation across every handler, where it would be
forgotten once.

## Consequences

`dispatch()` no longer returns the reply to its caller for local and remote
commands alike. A local command still fills its slot immediately; a cross-shard
command sends a request and returns.

**Slots stay single-threaded.** A cross-shard request travels to the owning loop,
which executes it against its own shard and sends the *reply* back to the
originating loop; that loop fills the slot. So a slot is created, filled and read
by one thread throughout, and the only thing that crosses a thread boundary is a
message that the queue hands over wholesale. This was worth arranging
deliberately: the alternative, letting the owning loop write into the
originator's slot, would have put shared mutable data on the request path — in
the middle of the one thing this architecture claims not to have.

Messages therefore address a connection by identifier rather than by pointer:
the pair (originating loop, per-loop counter), which is unique without any
shared counter, and never reused. A reply naming a connection that has since
gone is dropped. That is what keeps a dying connection from having to wait for
its outstanding replies: there is nothing for them to dangle against.

**A protocol error and `QUIT` take a slot like anything else.** Both used to be
written and closed on immediately, which with slots would let them overtake
replies the client is still owed. Instead each reserves a terminal slot, fills
it, and stops the connection reading; the connection closes when that slot is
flushed through the ordered prefix. So a client always receives every reply it
had coming before the error or the `+OK`, and only then the EOF.
