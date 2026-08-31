# Architecture

## What this is

One process, one port, `N` threads where `N` is the core count. Each thread owns
an epoll instance, a set of TCP connections, and one shard of the keyspace. The
thread that owns a shard is the only thread permitted to touch it, and the only
channel between threads is a message queue.

**Stated precisely, because the imprecise version is the interesting claim and
it is false:** no *data* is shared. Keys, values, connections, buffers and reply
slots each belong to exactly one thread for their whole lifetime, and nothing on
the request path is reachable from two threads at once. What is shared is the
message queues themselves and a single flag saying the server is shutting down —
lifecycle machinery, not state that a command reads or writes. Saying "threads
share no mutable state" would be a cleaner sentence and would not survive
someone reading the shutdown path.

The repository is responsible for the server and nothing else. There is no
client library, no cluster coordinator, no admin tool.

## Shape

```
                    +--------------------------------------+
   TCP connection ->| SO_REUSEPORT: the kernel assigns each |
                    | connection to one listening socket    |
                    +--------------------------------------+
                        |           |           |
                +-------+---+ +-----+-----+ +---+-------+
                |  Loop 0   | |  Loop 1   | | Loop N-1  |  one thread per shard
                |  epoll    | |  epoll    | |  epoll    |  (--pin adds CPU
                |  conns    | |  conns    | |  conns    |   affinity; off by
                |  Shard 0  | |  Shard 1  | | Shard N-1 |   default) 1/N of keys
                +-----------+ +-----------+ +-----------+
                      ^   |         ^   |        ^   |
                      |   +---------+---+--------+---+
                      +-------------+------------+
                     cross-shard requests: MPSC queue + eventfd
```

Each loop owns exclusively:

- one epoll instance
- a set of TCP connections - once a connection lands on a loop it never migrates
- one hash table holding 1/N of the keyspace

### Accepting connections

Every loop independently creates a socket, sets `SO_REUSEPORT`, binds and
listens on the same port. The kernel hands each incoming connection to one of
those listening sockets. There is no accept thread and no handoff: a connection
is born on the loop that will serve it for its whole life, which is why
connections never migrate.

**How the kernel chooses is not something this design controls, or should
assume.** `socket(7)` promises only that `SO_REUSEPORT` improves `accept(2)`
load distribution; it documents no algorithm, and the assignment can be
redefined outright with a BPF program. An earlier version of this document
asserted a 4-tuple hash -- a common description, but not one the manual makes.
Nothing here rests on it and no test asserts an even spread.

What the design relies on is weaker and is guaranteed: a connection is accepted
by exactly one loop and stays there. Uneven distribution costs throughput, not
correctness.

### Key to shard

`shard = hash(key) % N`, with a hash that avalanches. `std::hash` is not used for
strings, because its quality varies between standard library implementations and
it offers no avalanche guarantee.

The shard a key belongs to is unrelated to the loop its connection landed on.
Whether a given command stays local is chance, and the local fraction is
therefore about 1/N for uniformly distributed keys.

### The life of a request

`GET foo`, with the connection on Loop 2 and `foo` belonging to Shard 5:

1. Loop 2's epoll reports the connection readable; bytes go into that
   connection's reusable read buffer, with no allocation.
2. The parser works in place. The key and value it yields are `string_view`s
   into that buffer — nothing is copied yet.
3. `hash("foo") % N` is 5, so this is not the local shard.
4. A request message is built, and **the key is copied here**, because the read
   buffer may be reused before the reply comes back. It goes onto Loop 5's
   queue, and Loop 5's eventfd is written only if that queue was empty.
5. Loop 5 wakes, drains its queue, looks the key up in the table it exclusively
   owns — no lock — and sends the encoded reply back to Loop 2's queue.
6. Loop 2 fills the waiting slot and writes out the longest ready run, as
   described below.

Had `foo` belonged to Shard 2, step 3 would be followed straight by the lookup,
the encode and the write: **no thread crossed, no lock taken, no allocation
made.**

### Replies leave in order, whatever order they arrive in

RESP has no request identifiers: a client matches replies to commands by
position alone. But a pipelined run can mix local commands, which finish at
once, with cross-shard ones, which finish after a round trip — so replies
become ready out of order and must still go out in order.

Each connection therefore owns a queue of reply slots. Parsing a command
reserves a slot; a local command fills it immediately, a cross-shard one leaves
it empty until its reply returns. The connection writes out the longest filled
run from the front and stops at the first gap. Parsing does not pause for a gap,
so pipelining keeps working, and the stall is confined to the one connection
that caused it.

Slots never cross a thread. A cross-shard request goes to the owning loop, which
answers it against its own shard and sends the reply *back* to the originating
loop, and that loop fills the slot. Messages address a connection by an
identifier rather than a pointer, and identifiers are never reused: a reply for
a connection that has since closed is simply dropped, which is why a dying
connection never has to wait for replies still in flight toward it.

### A loop can only reach its own shard

Command execution goes through a `ShardRouter`, whose only accessor for a shard
is `local()`; everything else is a `send()`. So the rule that a thread touches
only its own partition is enforced by the type rather than by reviewers
remembering it, and because the interface is abstract, the ordering rules above
are testable with no threads and no sockets. See
`docs/adr/0008-routing-is-an-interface-so-ordering-can-be-tested.md`.

### Multi-key commands scatter and gather

`MGET`, `MSET`, `DEL` and `EXISTS` group their keys by shard, send one message
per remote group, and reassemble. **The reply is assembled by the arguments'
original positions, not by the order the groups came back in** — `MGET a b c`
answers about `a`, `b` and `c` in that order however the shards were scheduled.

`DBSIZE`, `FLUSHDB` and `INFO` fan out the same way. A `DBSIZE` that reported
only the local shard would not be slow, it would be wrong.

`INFO` is worth one note: it is itself a cross-shard command, so reading
`cross_shard_requests` through it adds to the number being read. That is not a
flaw in the counter -- requests sent are requests sent -- but a measurement
taken with `INFO` has to allow for the cost of the instrument.

## Boundaries

The process talks to nothing. No database, no queue, no other service, no other
repository in this project. Its only interface is the TCP port, and the contract
on that port is RESP2.

The external tools that matter are `redis-cli` (the client), `redis-benchmark`
(the load generator), and `redis-server` itself, which runs on the same machine
as the control group for every performance measurement.

## Protocol

RESP2. A request is an array of bulk strings - `SET foo bar` arrives as:

```
*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n
```

Inline commands (a bare `PING\r\n`) are also accepted, because `telnet` and some
tools send them.

Matching Redis's error behaviour matters as much as matching its framing, and
these are the details that get written wrong without checking: what `INCR`
returns for a non-numeric value, that `GET` on a missing key returns `$-1`
rather than an empty string, that `DEL` returns a count.

### An unknown command is an error reply, never a hang

Every command outside the implemented set is answered with
`-ERR unknown command 'X'`. The connection stays open and no bytes are left
unread.

This is not merely tidy: it is the protocol's documented downgrade path. The RESP
specification has clients open a session with `HELLO <version>`, and states that
a client detects a server that only speaks RESP2 by receiving
`-ERR unknown command 'HELLO'` and carrying on in RESP2. `HELLO` is therefore not
a command this server needs to implement, but it is one it must *refuse
correctly* — a server that hangs, closes, or silently ignores it fails the
handshake instead of declining it.

`COMMAND` is implemented rather than refused, returning an empty array. It is
cheap, and clients may ask for command metadata.

**The command name is escaped before it is quoted back.** An error reply is a
simple error, which is line-delimited, and the name in it came from the client
— a bulk string, so it may contain any byte at all. Echoing it raw is
response-line injection: `*1\r\n$14\r\nBAD\r\n+INJECTED\r\n` is a legal
request whose name would end the error line early and let the sender dictate
the bytes the client reads as its next reply. Every byte outside printable
ASCII becomes `\xHH`, and the echo is capped at 128 bytes.

The general rule this is an instance of: **client bytes may be returned inside
a bulk string, which is length-framed, but never inside a simple string or a
simple error, which are terminated by the first CRLF.**

### The parser is a pure function

**This is a hard requirement, not a preference.** Bytes in; out comes either a
parsed command plus the number of bytes consumed, or "need more data", or a
protocol error. It does not touch a socket. It does not touch the event loop.

That shape is what makes the most bug-prone part of the system directly
testable: feed it byte sequences and assert, with no network involved. Split
packets, coalesced packets, and hostile input are all just byte sequences.

## Memory and connection lifetime

### File descriptors are owned by an RAII type

Every fd lives in a move-only `UniqueFd` that closes on destruction. Raw
`close()` calls do not appear in the codebase. A descriptor leak is the failure
mode that survives a one-hour soak test unnoticed and then exhausts the process
in production, so ownership is made structural rather than remembered.

### Buffers belong to the connection

Each connection owns one read buffer and one write buffer, reused across
requests rather than reallocated per request. On the common path -- a single-key
command that lands on the local shard -- a steady-state request causes no heap
allocation at all.

**The consumed prefix is reclaimed; the capacity is not.** A buffer resets when
it drains completely, and otherwise compacts -- moving the unread tail to the
front -- once the consumed prefix is both at least 8 KiB and at least half the
buffer. The byte floor keeps the request-at-a-time path free of `memmove`; the
half condition bounds the cost, since a compaction moves no more bytes than it
discards.

What it does **not** do is hand the allocation back: a connection keeps a buffer
as large as the largest burst it has served, and reuses it. That distinction is
the one to have in mind when reading an RSS graph -- a buffer no longer grows
with the *number* of requests served, which had no bound, but does sit at the
size of the *biggest*, which does. Compaction moves bytes, so it invalidates
`readable()`, which was always the contract. See
`docs/adr/0011-buffers-compact-their-consumed-prefix-but-keep-their-capacity.md`.

### Backpressure

A client that pipelines heavily and never reads its responses would grow the
write buffer without bound if nothing stopped it. Two watermarks stop it. At or
above 1 MiB of pending output the connection stops being read from -- `EPOLLIN`
is dropped, the kernel's receive buffer fills, and TCP flow control pushes back
on the sender. At or below 256 KiB, reading resumes. Two rather than one, because
a single threshold oscillates at the boundary.

**No client is ever closed for being slow, and there is no output-buffer kill
limit.** Once reading stops, no further command is parsed, so the buffer cannot
grow past the replies to what was already read: the growth was unbounded because
the parse was, and bounding one bounds the other. See
`docs/adr/0010-backpressure-is-two-watermarks-and-never-a-disconnect.md`.

**A paused connection always still wants to write** -- the invariant that keeps
backpressure from wedging a client. Interest is computed from `wantsRead()` and
`wantsWrite()`, so were both false the connection would be registered for no
events and never woken again. Pausing needs 1 MiB pending and resuming happens at
256 KiB, so while paused `EPOLLOUT` is always registered. A test pins it.

In `INFO`, a loop's `short_writes` climbing says a client is not keeping up, and
`read_pauses` says backpressure actually engaged.

### A cross-shard MSET is not atomic

Redis applies an `MSET` as one indivisible step. Here the writes land on
different threads at different moments, so a concurrent `MGET` can see some of
them and not others. It follows from the architecture rather than being an
oversight -- atomicity would need either a lock spanning shards, the very thing
this design exists to avoid, or a two-phase commit serving one command. Recorded
as a non-goal in `docs/product.md` and as a limitation in the README, because it
is the kind of difference that should be volunteered rather than discovered.

### Cross-shard messages own their keys

A cross-shard request cannot borrow from the originating connection's read
buffer, which may be reused before the reply arrives. The message therefore owns
a copy of the key. This copy is the concrete price of the shared-nothing design
and it is paid only on the cross-shard path.

## Data structures

**Hash table.** v1 uses one `std::unordered_map<std::string, Value>` per shard:
the standard library first, a hand-written open-addressing table later if the
measurements justify it. Writing the replacement without a baseline to compare
it against would produce a table with no story attached.

**Values.** Byte strings, and only byte strings. `INCR` parses the stored text
and formats the result back. Caching an integer representation alongside the
string is an obvious optimisation and is deliberately not taken in v1 — see the
decision record, which is the same argument as for the hash table: the
optimisation is cheap to add once a profile asks for it, and the correctness
traps it introduces (`SET k 001` must still `GET` as `001`) are expensive to find.

**Expiry.** Lazy plus sampled. An expired key is dropped when it is next
accessed, in `Shard::lookup`, which is why no command can observe one. Lazy alone
would leak the keys nobody reads again, so each loop's timer also runs bounded
sampling passes over its own shard, sweeping the table on a cursor so that a lap
without an intervening rehash reaches every key. `DBSIZE` still does not sweep -- it reports
the table as it stands; what changed is that the table converges. The pass lives
on `Shard` and takes its time from the shard's `Clock`, so tests drive the whole
of it by hand rather than by sleeping. See
`docs/adr/0012-a-timerfd-per-loop-drives-sampled-expiry.md`.

**Eviction.** LRU is roadmap, not v1.

Each shard's structures are touched by exactly one thread, so none of them need
to be thread-safe, and none of them are.

## Conventions that are not obvious from the code

**Level-triggered epoll, not edge-triggered.** Getting level-triggered wrong
costs a redundant wakeup; getting edge-triggered wrong costs a silently wedged
connection. The reasoning, and the conditions under which ET would be
reconsidered, are in `docs/adr/0009-level-triggered-epoll-not-edge-triggered.md`.

**`EPOLLOUT` is registered on demand.** It is not held permanently: under LT a
permanently-registered writable socket spins the event loop. It is registered
only when a `write()` comes up short and deregistered as soon as the buffer
drains.

**Each loop owns a timerfd with two duties.** It ticks every 100 ms in the
loop's own epoll set -- so `epoll_wait` keeps its infinite timeout and the tick is
just another descriptor to dispatch on -- and it both runs the expiry sampling
pass and re-arms a throttled listener.

**A listener out of descriptors is throttled, not retried.** `EAGAIN` from
`accept()` means the backlog is empty and the loop can sleep; `EMFILE` means it is
*not* empty and there was no descriptor to accept into, so retrying under
level-triggered epoll is an unbreakable spin. The loop drops `EPOLLIN` from the
listener and the next tick puts it back, so the waiting client sits in the
backlog until this loop has a descriptor again rather than being refused -- at
least a tick, and possibly longer, because dropping the interest does not take
the listener out of its `SO_REUSEPORT` group and the kernel goes on assigning to
it. `loopN_accept_failures` says it happened. See
`docs/adr/0013-a-listener-out-of-descriptors-is-throttled-not-retried.md`.

**The build environment is the container, not the developer's machine.** The
repository carries a `Dockerfile` with the toolchain, CMake and `redis-tools`;
the suite, the sanitizer builds and the manual protocol checks all run inside it,
so that a result here and a result in CI are the same statement rather than two
that happen to agree. The reasoning is in `docs/adr/`.

**Sanitizers are part of the build matrix, not an occasional check.** CI builds
three ways on `ubuntu-latest`: Release for benchmarking and regressions, Debug
with ASan and UBSan for memory and undefined behaviour, and Debug with TSan for
data races.

The TSan build is wired up while the server is still single-threaded, where it
has nothing to find. That is deliberate: the threading work then lands into a
pipeline that already reports races, rather than being written for days and
audited afterwards.

**The TSan build needs two accommodations on modern kernels**, both already made in CMake and CI. They look like broken tooling the first time they are met, so the symptom and the reasons are in `docs/adr/0003-build-and-test-in-a-container.md`.

TSan is the one that matters. The correctness of this architecture reduces to a
single claim -- no two threads touch the same data -- and TSan checks that claim
mechanically rather than by inspection. It must cover the cases where the claim
is most likely to break: many clients reading and writing keys that span shards,
connections opening and closing while requests are in flight, and shutdown with
messages still in transit. **A race here means a piece of state escaped its
thread; the answer is to find which one, never to add a mutex.**

**Tests are expected to be able to fail.** Beyond unit tests, the suite includes
protocol conformance against real client behaviour, and fault injection: a slow
client that never reads, a client killed mid-write, `accept()` under an
exhausted `ulimit -n`, and a soak run watched for RSS and fd growth. Error paths
are covered, not only the happy path.

**Performance numbers are inseparable from their environment.** Every recorded
figure carries the machine, kernel, compiler, optimisation level, whether load
was generated locally and whether threads were pinned; measurements live in
`benchmarks/` beside the script and raw output, with the expected result written
down before the run. `docs/product.md` states the principle; this is where it
lands in practice.

**What this machine cannot measure is recorded rather than skipped.** The
available machine is virtualised and its PMU is unreachable, so there is no
scaling curve and no profile, and no claim about scaling with cores is made
anywhere. What replaces them is a same-machine Redis control group, where the
difference is claimed rather than the absolutes, and the mechanism evidence from
the sharding work -- keys demonstrably spread, the cross-shard path demonstrably
taken, TSan silent under load. That evidence says shared-nothing is implemented
and race-free; it says nothing about how well it scales, and the two are not
allowed to blur. See
`docs/adr/0014-what-this-machine-can-and-cannot-measure.md`.

## Toolchain

| Layer      | Choice              | Why                                              |
|------------|---------------------|--------------------------------------------------|
| Language   | C++20               | concepts, `span`, `string_view`, designated initialisers |
| Build      | CMake               | the default; needs no explanation                |
| Tests      | GoogleTest          | the default                                      |
| Sanitizers | ASan / UBSan / TSan | ship with the compiler; no dependency            |
| Hashing    | xxHash              | single embeddable file, good avalanche           |
| I/O        | epoll               | see below                                        |
| Platform   | Linux               | `SO_REUSEPORT` and eventfd are Linux facilities  |

**Not io_uring.** It is newer and faster, but it demands recent kernels, its
ecosystem is thinner, and epoll is the interface most readers of this code will
already know. The project is about the concurrency architecture above the I/O
interface, and epoll keeps that the subject.

## Where decisions live

Decision records are in `docs/adr/`, one decision per file. Search them rather
than reading the directory.
