# Buffers compact their consumed prefix but keep their capacity

summary: A connection buffer moves its unread tail down once the consumed prefix is both large and at least half the buffer; the allocation itself is never handed back, so a buffer stays as large as the biggest burst it has served.

## Context

A `Buffer` is a `std::string` plus a `start_` offset marking how much has been
consumed. Until now it reset only when it drained completely: any unread byte
behind the offset kept the whole consumed prefix alive. A connection that
pipelines continuously never drains completely, so its buffer grew with the
traffic through it rather than with the traffic in it.

Two separate things were on the table and they are easy to conflate:

- **Compaction** -- move the unread tail to the front and reset the offset. This
  reclaims the consumed prefix for reuse.
- **Shrinking** -- return the allocation to the allocator, so the process's
  resident memory falls after a burst.

## Decision

**Compaction, on a two-part condition.** In `consume()`, after the existing
"fully drained, clear it" case, compact when the consumed prefix is both at
least 8 KiB and at least half the buffer.

**No shrinking.** `std::string::erase(0, n)` keeps the capacity, and nothing is
added to give it back -- no `shrink_to_fit`, no swap with a smaller string, no
idle-time reclaim. A connection keeps an allocation as large as the largest
burst it has served, for its whole life.

## Reasoning

**The property worth buying is bounded, not small.** The defect was that the
buffer grew with the number of requests a connection had served, which has no
bound. After compaction it grows with the size of one burst, which does. That is
the whole of the problem; the remaining peak-sized allocation is reused on every
subsequent request and never grows again.

**The byte floor keeps the common path free of `memmove`.** A connection serving
one request at a time consumes its whole buffer and hits the drained case, never
this one. A pipelined connection would otherwise pay a move per command for a
few dozen bytes -- work in exchange for nothing, on the path the architecture
exists to keep cheap.

**The half condition is what bounds the cost.** Compacting moves at most as many
bytes as it discards, so each byte is moved a bounded number of times over its
life in the buffer, whatever the traffic pattern. On the byte floor alone, a
buffer holding a large unread tail behind a just-over-8-KiB prefix would be
moved again and again to reclaim very little.

**Shrinking trades a bounded, reused allocation for repeated allocation.** The
memory a shrink returns is memory the next burst will ask for again, and the
connections that hold large buffers are exactly the ones whose traffic justified
them. The v1 answer is that a per-connection allocation sized by that
connection's peak is acceptable and predictable. If a soak run ever shows RSS
held up by buffers on connections that have gone quiet, that measurement is what
should reopen this -- the same shape as
`docs/adr/0001-use-the-standard-library-hash-table-first.md`, and for the same
reason.

**The documents must say which one was done.** "Buffers only grow" was true and
is now false; "buffers no longer hold their peak allocation" would be false in
the other direction. `docs/architecture.md` and the README say that the
consumed prefix is reclaimed and the capacity is not, because the difference is
exactly what someone reading an RSS graph needs to know.

## Consequences

`readable()` and every `string_view` into it are invalidated by a compacting
`consume()`, where before they survived it. This was already the documented
contract and `Connection::drainInput()` already honoured it -- the parser's
views are never touched after `read_.consume()` -- so no call site changes. What
changes is that breaking the rule now has a consequence, which is an argument
for the rule rather than against the change.
