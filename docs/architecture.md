# Architecture

This describes the finished shape. Part of it is built and part of it is not,
and the difference matters when reading the rest: **the server today runs one
loop on one thread with one shard.** Everything about accepting connections,
parsing, storing and replying is written and tested. Everything about several
loops — the cross-shard queue, the eventfd wakeups, CPU affinity, the
scatter/gather multi-key commands — is designed here but not yet implemented.

The pieces that exist were built in the shape the rest needs, so what comes
next is constructing N of them rather than rewriting these.

## What this is

One process, one port, `N` threads where `N` is the core count. Each thread owns
an epoll instance, a set of TCP connections, and one shard of the keyspace. The
thread that owns a shard is the only thread permitted to touch it. Threads share
no mutable state; the only channel between them is a message queue.

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
                |  Loop 0   | |  Loop 1   | | Loop N-1  |  one thread per core,
                |  epoll    | |  epoll    | |  epoll    |  pinned by CPU affinity
                |  conns    | |  conns    | |  conns    |
                |  Shard 0  | |  Shard 1  | | Shard N-1 |  1/N of the keyspace
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
listens on the same port. The kernel hashes each incoming connection's 4-tuple
and hands it to one of the listening sockets. There is no accept thread and no
handoff: a connection is born on the loop that will serve it for its whole life.

This is why connections never migrate. It also means connection distribution is
the kernel's business, and can be uneven for a small number of clients - with
`redis-benchmark -c 50` it evens out.

### Key to shard

`shard = hash(key) % N`, with a hash that avalanches. `std::hash` is not used for
strings, because its quality varies between standard library implementations and
it offers no avalanche guarantee.

The shard a key belongs to is unrelated to the loop its connection landed on.
Whether a given command stays local is chance, and the local fraction is
therefore about 1/N for uniformly distributed keys.

### The life of a request

`GET foo`, with the connection on Loop 2 and `foo` belonging to Shard 5:

1. Loop 2's epoll reports the connection readable.
2. Bytes are read into the connection's own reusable read buffer. No allocation.
3. The RESP parser parses in place. The key and value it produces are
   `string_view`s into that read buffer - nothing is copied.
4. `hash("foo") % N` is 5, which is not this loop's shard.
5. A cross-shard request message is built. **The key is copied here**, because
   the read buffer may be reused before the response comes back. The message is
   pushed onto Loop 5's MPSC queue and its eventfd is written to wake it.
6. Loop 5 wakes, takes the request off its queue, looks the key up in the hash
   table it exclusively owns - no lock - and pushes the result onto Loop 2's
   return queue, waking Loop 2.
7. Loop 2 encodes the result as RESP into the connection's write buffer.
8. It attempts `write()` directly. If the write comes up short, it registers
   `EPOLLOUT` and sends the remainder when the socket is writable.

Had `foo` belonged to Shard 2, step 4 would be followed directly by the lookup,
the encode and the write: **no thread crossed, no lock taken, no allocation
made.**

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
requests. They grow when they must and are compacted when the consumed prefix
gets large, rather than being reallocated per request. On the common path - a
single-key command that lands on the local shard - a steady-state request causes
no heap allocation at all.

As with expiry, this lands in two parts. The buffers are reused from the start;
**compaction of the consumed prefix, and the backpressure watermarks below,
arrive later with the rest of the resource management.** Until then a buffer only
ever grows, so a long-lived connection issuing many commands holds a read buffer
as large as its largest burst, and a client that never reads its replies grows
the write buffer without bound. Both are known gaps of that interval, not
oversights.

### Backpressure

A client that pipelines heavily and never reads its responses will grow the
write buffer without bound if nothing stops it. Two watermarks do:

- above the high watermark, the connection stops being read from. `EPOLLIN` is
  deregistered, so the kernel's receive buffer fills and TCP flow control pushes
  back on the sender
- once the write buffer drains below the low watermark, reading resumes

Two watermarks rather than one, because a single threshold oscillates.

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

**Expiry.** Lazy plus sampled: an expired key is dropped when it is next
accessed, and a background sample per loop clears keys that are never touched
again. Lazy alone leaks memory for keys nobody reads.

The two halves land separately. The lazy half ships with the expiry commands;
the sampling half comes later, with the rest of the resource management. Until
it does, **an expired key that is never accessed again is never freed** — a known
and accepted gap, not an oversight. `Value` carries its expiry deadline from the
start so that closing the gap does not change the data structure.

**Eviction.** LRU is roadmap, not v1.

Each shard's structures are touched by exactly one thread, so none of them need
to be thread-safe, and none of them are.

## Conventions that are not obvious from the code

**Level-triggered epoll, not edge-triggered.** ET produces fewer events but
requires reading until `EAGAIN` every time; one missed read silently wedges a
connection, and that class of bug is extremely hard to reproduce. LT's "still
readable, telling you again" semantics tolerate the mistake. With pipelining the
event-count difference is small, because one read usually yields several
commands. If profiling later shows LT's event overhead to be material, the read
path can move to ET with a read-until-`EAGAIN` loop - that is a roadmap item
with a measurement attached, not a v1 decision.

**`EPOLLOUT` is registered on demand.** It is not held permanently: under LT a
permanently-registered writable socket spins the event loop. It is registered
only when a `write()` comes up short and deregistered as soon as the buffer
drains.

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

**Running the TSan build needs two accommodations**, both already made, and both
recorded here because they look like broken tooling the first time they are met.
ThreadSanitizer requires a particular address-space layout and refuses to start
when mmap randomisation uses the 32 bits of entropy that Ubuntu 24.04 kernels
default to — the symptom is `FATAL: ThreadSanitizer: unexpected memory mapping`
before any test runs. So:

- CMake runs the test binaries under `setarch -R` for the thread build only. It
  is attached as `CROSSCOMPILING_EMULATOR` rather than `gtest_discover_tests`'s
  `LAUNCHER`, because discovery also executes the binary, to enumerate cases,
  and `LAUNCHER` does not cover that run.
- `setarch -R` calls `personality()`, which Docker's default seccomp profile
  blocks, so the thread build needs `--security-opt seccomp=unconfined`. CI has
  no such profile and instead lowers `vm.mmap_rnd_bits` directly, which is the
  sturdier fix where it is available.

TSan is the important one. The correctness of this architecture rests entirely
on the premise that no two threads touch the same data, and TSan is the only
tool that checks the premise mechanically rather than by inspection. The
scenarios it must cover are the ones where the premise is most likely to break:
many clients reading and writing the same keys so that cross-shard traffic is
generated, connections opening and closing while requests are in flight, and
shutdown with cross-shard messages still in transit.

**Tests are expected to be able to fail.** Beyond unit tests, the suite includes
protocol conformance against real client behaviour, and fault injection: a slow
client that never reads, a client killed mid-write, `accept()` under an
exhausted `ulimit -n`, and a soak run watched for RSS and fd growth. Error paths
are covered, not only the happy path.

**Performance numbers are inseparable from their environment.** Every recorded
figure carries CPU model and core count, kernel version, compiler and
optimisation level, whether load was generated on the same machine, and whether
threads were pinned. Measurements live in `benchmarks/` with the script that
produced them and the raw output, and the expected result is written down before
the run, so that it is an experiment rather than a demonstration.

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
