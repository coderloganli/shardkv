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
    && rm -rf /var/lib/apt/lists/*

# g++-13 for complete C++20 support. Pinned as the default so that CMake, the
# sanitizer builds and CI all agree on one compiler.
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

# redis-tools above is the reason this is a container rather than a bare
# toolchain: the protocol conformance work needs a real redis-cli and a real
# redis-benchmark, at a version that is pinned rather than whatever the host
# happens to have.

WORKDIR /src
