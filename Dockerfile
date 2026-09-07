# Build and test environment (docs/adr/0003-build-and-test-in-a-container.md).
#
# The server is Linux-only by design -- epoll, SO_REUSEPORT and eventfd have no
# portable equivalents -- so the build, the test suite and the manual protocol
# checks all happen in here rather than on the developer's machine.
#
#   docker build -t shardkv-dev .
#   docker run --rm -v "$PWD":/src -w /src shardkv-dev \
#       bash -c 'cmake -B build -G Ninja && cmake --build build && ctest --test-dir build --output-on-failure'

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      g++-13 \
      cmake \
      ninja-build \
      git \
      ca-certificates \
      redis-tools \
      redis-server \
      linux-tools-common \
      linux-tools-generic \
    && rm -rf /var/lib/apt/lists/*

# g++-13 for complete C++20 support. Pinned as the default so that CMake, the
# sanitizer builds and CI all agree on one compiler.
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

# redis-server is here for the control group. Every performance figure in
# benchmarks/ is a comparison against Redis on the same machine under the same
# load, which is what makes a difference meaningful on a virtualised host where
# the absolute numbers are not -- see
# docs/adr/0014-what-this-machine-can-and-cannot-measure.md. It comes from the
# same Ubuntu package set as redis-tools, so the control group and the load
# generator are the same version, and benchmarks/environment.sh refuses to
# record a run where they are not. It does not start on its own: the image
# installs it, and the benchmark scripts run it on a port of their choosing.
#
# redis-tools above is the reason this is a container rather than a bare
# toolchain: the protocol conformance work needs a real redis-cli and a real
# redis-benchmark, at a version that is pinned rather than whatever the host
# happens to have.

# linux-tools is perf, for benchmarks/profile.sh. Two things about it that will
# otherwise be rediscovered painfully:
#
# The `perf` on PATH is a wrapper that execs the build matching `uname -r`, and
# in a container that is the HOST's kernel version, which is not installed here.
# So the wrapper fails and the real binary lives at /usr/lib/linux-tools/*/perf.
# profile.sh looks there; nothing needs fixing, but the first person to run
# `perf` by hand will think it does.
#
# And perf_event_open is blocked by Docker's default seccomp profile, so a
# profile needs --security-opt seccomp=unconfined -- the same flag the thread
# sanitizer build already needs, for a different syscall. Without it perf says
# "No permission to enable task-clock event", which reads as a kernel setting
# and is not one. profile.sh prints the candidates rather than guessing.
#
# NOTE: this package is deliberately NOT added to .github/workflows/ci.yml,
# which keeps its own apt list. CI runners will not sample anyway, and
# benchmarks/self_test.sh asserts profile.sh's REFUSAL there rather than
# skipping the case -- both outcomes are definite. The two lists usually have to
# change together; this is the exception, and it is written down here so it
# reads as a decision rather than as the omission it looks like.

WORKDIR /src
