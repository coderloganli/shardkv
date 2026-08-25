# Architecture

Everything below is design. No code exists yet, so treat this as the intended
shape rather than a description of what is there.

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
single-key command that lands on the local shard - a request causes no heap
allocation at all.

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

**Values.** Strings, with integers stored in a form that lets `INCR` avoid
reparsing on every operation.

**Expiry.** Lazy plus sampled: an expired key is dropped when it is next
accessed, and a background sample per loop clears keys that are never touched
again. Lazy alone leaks memory for keys nobody reads.

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

**Sanitizers are part of the build matrix, not an occasional check.** CI builds
three ways on `ubuntu-latest`: Release for benchmarking and regressions, Debug
with ASan and UBSan for memory and undefined behaviour, and Debug with TSan for
data races.

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
