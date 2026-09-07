# The reply queue is a ring, not a deque

summary: Each connection's ordered reply queue is a ring buffer over one growable allocation rather than a std::deque, because the deque allocated a node every six commands and freed one every six replies for the life of every connection — a per-request cost that no steady state amortised away, and the whole of the gap between this server and its own claim that a common command allocates nothing.

## Context

Replies leave a connection in command order whatever order they become ready in,
which is what `0006-replies-are-written-into-ordered-slots.md` is about. The
queue that does it is a first-in-first-out of slots: one pushed when a command is
parsed, one dropped when its reply goes on the wire. It was a `std::deque`,
which is the obvious container for exactly that shape.

The product document also claims that a common command performs no per-request
heap allocation. Nothing checked that, and it was not true.

A test that counts calls to global `operator new` put a number on it: 167
allocations per thousand `PING`s, and the same 167 for a local `GET`, on a
single shard, after warm-up, with the count rising exactly linearly. The same
figure for a command that touches the store as for one that does not, which
places the cost on the path every request shares rather than in the store.

One in six is not a coincidence. `sizeof(Slot)` is 80 bytes, libstdc++ sizes a
deque's node at 512, and 512 divided by 80 is six. The queue was allocating a
fresh node every sixth command and freeing one every sixth reply, forever.

**Steady state does not help, and that is the part worth keeping.** A deque
whose queue stays one or two deep still walks forward through its nodes: it
allocates at the back and frees at the front, and the two never meet. Depth was
never the problem; direction of travel was.

## Decision

**A ring buffer over a single `std::vector<Slot>`,** with a head index and a
live count. Reserve writes at the back; the drain resets the front cell and
steps the head. Slot numbers keep rising for the life of the connection and
`base_` keeps the offset, exactly as before.

**It grows by doubling and never shrinks.** A connection that once queued deeply
keeps the room to do it again. This is the same bargain the read and write
buffers already strike, for the same reason, and it is recorded in
`0011-buffers-compact-their-consumed-prefix-but-keep-their-capacity.md`. The
memory is bounded by the deepest burst the connection ever had; in exchange, the
steady state allocates nothing at all.

**A drained cell is reset, not merely stepped over.** The cell holds the reply's
bytes. A ring that kept them would hold every reply the connection had ever
sent, which would trade a small recurring allocation for an unbounded one.

## Reasoning

**The measurement chose the target; intuition would not have.** The candidate
before this was a red-black tree lookup on the command name, which is visible in
the sampling profile and looks like the obvious thing to fix. It costs about two
per cent of user-space time. This cost a hundred per cent of a claim the project
had published, and nothing in the profile pointed at it — the allocator's own
symbols did, and only the allocation counter said what they were for.

**A container chosen for its interface was paying for its layout.** `std::deque`
is the textbook answer for a queue with pushes at one end and pops at the other,
and its interface is exactly right here. What it does underneath — hand out
fixed-size nodes — is not free, and nothing about the interface says so. The
replacement uses more code and less machinery.

**The alternative was to weaken the claim instead**, which would have been the
honest response to a cost that could not be removed. It could be removed, in
about forty lines, so it was.

## Consequences

`PING`, and a local single-key `GET` of a value small enough to live inside its
own `std::string`, now allocate nothing per request. `tests/alloc_test.cc`
asserts that, so the claim is checked rather than believed.

**What is left is stated rather than rounded off.** A reply whose bytes exceed
the small-string buffer still costs two allocations: the shard encodes into a
`std::string`, and a large one takes a block and gives it back. Removing those
means encoding directly into the connection's write buffer, so that dispatch
stops returning a reply and starts writing one — a change to the shape of the
program and to the ordered-slot design, not a tidy-up. It is not done here, and
`docs/product.md` says so where it makes the claim.

A single-key command whose key belongs to another shard is outside the claim
altogether and always was: it must copy its arguments, because the read buffer
they point into is reused before the reply returns. At eight shards that is
seven requests in eight.
