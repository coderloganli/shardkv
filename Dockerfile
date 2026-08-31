# Build and test environment (docs/adr/0003-build-and-test-in-a-container.md).
#
# The server is Linux-only by design -- epoll, SO_REUSEPORT and eventfd have no
# portable equivalents -- so the build, the test suite and the manual protocol
# checks all happen in here rather than on the developer's machine.
#
#   docker build -t shardkv-dev .
#   docker run --rm -v "$PWD":/src -w /src shardkv-dev \
#       bash -c 'cmake -B build && cmake --build build -j && ctest --test-dir build --output-on-failure'

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

WORKDIR /src
