# Benchmarks

Four measurements, each against `redis-server` on the same machine under the same
load, each recorded with the environment that makes it mean something.

Read `predictions.md` first. It was written and committed **before** any of these
ran, which is what makes it an experiment rather than a demonstration — and the
git history is the only thing that can prove it, since no test can tell when a
sentence was thought of.

Read `docs/adr/0014-what-this-machine-can-and-cannot-measure.md` second. It says
what is deliberately absent here and why: there is **no scaling curve and no
profile**, because the machine is virtualised and its PMU is unreachable, and no
claim about scaling with cores is made anywhere.

## Running them

Inside the container, which is where the toolchain and both servers live:

```
docker build -t shardkv-dev .
docker run --rm -v "$PWD":/src -w /src shardkv-dev bash -c \
  'cmake -B build -G Ninja && cmake --build build && benchmarks/run-all.sh'
```

Each script is runnable on its own and writes its own results directory.

| script | what it measures |
|---|---|
| `throughput.sh` | `-t get,set -n 1000000 -c 50`, both servers: with and without `-P 16`, and with the generator single-threaded (as §8.1 specifies) and given `--threads` |
| `latency.sh` | the distribution at one connection with no pipelining, both servers |
| `cross_shard.sh` | what an extra hop between threads costs a request |
| `memory.sh` | resident memory after a million keys, both servers |
| `run-all.sh` | the four of them into one directory |

The sizes come from the technical document's §8.1 and are overridable:
`BENCH_REQUESTS`, `BENCH_CLIENTS`, `BENCH_SHARDS`, `BENCH_PIPELINE`,
`BENCH_ROUNDS`, and — because they are not throughput measurements and would
otherwise inherit a million requests each — `BENCH_LATENCY_REQUESTS` and
`BENCH_CROSS_REQUESTS`. Small values make the whole set run in seconds, which is
how `bench_smoke` exercises it in CI.

## What a results directory contains

```
results/<UTC timestamp>/
  environment.txt      the machine, kernel, compiler, both redis versions, ...
  throughput.raw       redis-benchmark's own output, unedited
  throughput.txt       the figures pulled out of it
  latency.raw / .txt
  cross_shard.raw / .txt
  memory.raw / .txt
```

**A results directory that exists is a complete one.** No script writes into
`results/` at all: each builds in a temporary directory whose first file is the
environment block, and moves it into place only when every step has succeeded. A
failure anywhere leaves nothing behind. That is a property of the mechanism
rather than of anyone's discipline, and `self_test.sh` asserts it for each script
separately.

`environment.sh` fails rather than emitting a blank field, and refuses outright
when `redis-server` and `redis-benchmark` are different versions — a control
group on a different version is a different experiment from the one being
claimed.

## The one measurement that needed designing

The cross-shard penalty cannot be arranged. `redis-benchmark` generates
`key:__rand_int__` and cannot be aimed at a shard — `shard_keys` solves that half
by reusing the server's own `shardForKey` — and **which loop accepts a connection
is the kernel's business**, which this design neither controls nor assumes.

So each run is classified after the fact. `INFO` reports
`loopN_cross_shard_requests`; bracket a single-connection run with two readings
and the delta says what happened: near zero and the connection's loop happened to
own the shard, near the request count and every request crossed a thread.

Two details that are not decoration:

- **The threshold is half the request count, not zero.** `INFO` is itself a
  cross-shard command and each reading adds about `shards - 1` to the counter it
  is reporting. Half the request count is unreachable by any instrument cost —
  provided the run is large enough, which is why classification refuses below
  `100 * shards` requests.
- **The report carries the spread, and refuses below three samples per group.**
  Each run is local with probability `1/shards`, so the local group is the small
  one. A difference of medians from one sample is not a difference, and ten
  samples on a noisy virtual machine deserve to be judged rather than trusted —
  hence the counts, minima and maxima, and the `ranges_overlap` flag.

`docs/adr/0015-the-cross-shard-penalty-is-observed-not-arranged.md` has the rest.

## The generator is part of the experiment

`redis-benchmark` is single-threaded unless `--threads` says otherwise, so
`-c 50` is fifty connections through one client thread — and that thread
saturates around 45,000 requests a second, before eight loops do. Measured as
§8.1 writes it, shardkv therefore comes out slower than Redis, which is a fact
about the benchmark rather than about the server.

`throughput.sh` runs it both ways and records both, under separate names
(`..._p1_...` and `..._p1_t8_...`). Neither is the true number: the first is
the instrument's ceiling, the second takes cores from the server on a machine
that has eight. Reporting only one of them would be reporting a choice.

## Reading the numbers

**The absolute figures are depressed and the differences are the claim.** Eight
cores, and `redis-benchmark -c 50` competes with the server for all of them. The
control group runs under exactly the same competition, so what the contention
does to one it does to the other.

**Nothing here is a scaling result.** A comparison at one shard count is one
point, not a curve.
