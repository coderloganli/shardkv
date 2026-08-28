# Build and test in a container

summary: The repository carries a Dockerfile, and the build, the test suite and the manual protocol checks all run inside it rather than on the developer's machine.

## Context

The server is Linux-only by design: `epoll`, `SO_REUSEPORT` and `eventfd` have no
portable equivalents, and the architecture is built on all three. The machine
this is developed on runs Windows, which has none of them and no CMake either.

So a Linux environment is required. The question is which one, and the answer has
to hold for three different things: the compile-and-test loop during
development, the sanitizer builds, and the manual protocol checks against
`redis-cli` and `redis-benchmark` — which need those binaries present, not just a
compiler.

## Decision

The repository carries a `Dockerfile` providing Ubuntu, a C++20 toolchain, CMake,
GoogleTest's dependencies and `redis-tools`.

`config.json`'s `test` command runs the suite inside a container built from it.
The sanitizer builds use the same image. The manual protocol checks use the
`redis-cli` and `redis-benchmark` from the same image, so that the client
exercising the server is a known version rather than whatever the host happens to
have.

## Reasoning

**The environment is part of what is being verified.** This project's claims are
about behaviour under sanitizers and about performance, and both are properties
of a toolchain and a kernel as much as of the source. An image that CI can build
from the same file makes "it passes here" and "it passes in CI" the same
statement. With a hand-configured environment they are two statements that
happen to agree until they do not.

**`redis-tools` is the part that settles it.** The protocol conformance work
needs a real `redis-cli` and a real `redis-benchmark` — that is the entire point
of choosing RESP. Pinning them in the image means a conformance failure is a
fact about the server rather than a question about which client version was
installed.

**The alternative considered was WSL2**, which is already installed here and
would have lower per-run latency: no container start, and no image to rebuild
when a dependency changes. It was rejected because a WSL distribution is
hand-configured state that drifts. Installing `redis-tools` into it is a step
someone has to remember, and the sanitizer results would be produced by whatever
compiler that distribution happens to carry, which is not the compiler CI uses.

**The cost is accepted and is real:** every test run pays container startup, and
the compile-and-test loop is slower than it would be natively. This is the wrong
trade for a project whose inner loop is the bottleneck; it is the right one here,
because the number of test runs is small relative to the cost of an environment
question contaminating a sanitizer or benchmark result.

**A third option — keeping the checkout inside the WSL filesystem — was rejected
for an unrelated reason:** it would move the repository out of the project's
`main/<repo>` workspace layout, which is where the surrounding task tooling
expects it.

## Running the ThreadSanitizer build

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

## Consequence for the performance step

`perf` and flame graphs are constrained inside containers: they need elevated
capabilities to read hardware counters, and inside WSL2 the counters may not be
available at all. This is not a problem for the work covered by this decision —
building, testing and sanitizing — but the measurement step will have to resolve
it, and the likely answer is a Linux host rather than this machine. Recording it
here so that it is a known open question rather than a surprise.
