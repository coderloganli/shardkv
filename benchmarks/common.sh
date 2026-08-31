#!/usr/bin/env bash
#
# Shared by the four measurement scripts: starting and stopping the two servers,
# and the rule that makes "no figures without their environment" a property of
# the mechanism rather than something four scripts each have to remember.
#
# The rule works like this. No script writes into benchmarks/results/ at all.
# Each builds its output in a temporary directory whose FIRST file is the
# environment block, and only when every step has succeeded is that directory
# moved into place under its timestamp. A failure anywhere -- including in
# environment.sh -- leaves nothing behind. So a results directory that exists is
# a complete one, and that is checkable rather than trusted.

BENCH_HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_REPO="$(cd "${BENCH_HERE}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${BENCH_REPO}/build}"
BENCH_RESULTS="${BENCH_HERE}/results"

# §8.1's figures by default; smaller values make the scripts exercisable in a
# few seconds, which is how the smoke test runs them. The same shape
# scripts/soak.sh already uses.
BENCH_REQUESTS="${BENCH_REQUESTS:-1000000}"
BENCH_CLIENTS="${BENCH_CLIENTS:-50}"
BENCH_ROUNDS="${BENCH_ROUNDS:-10}"
BENCH_SHARDS="${BENCH_SHARDS:-$(nproc)}"
BENCH_PIPELINE="${BENCH_PIPELINE:-16}"

# Ports well away from 6379 and 6380, so a soak or a manual session running
# alongside is not measured by accident.
BENCH_SHARDKV_PORT="${BENCH_SHARDKV_PORT:-6491}"
BENCH_REDIS_PORT="${BENCH_REDIS_PORT:-6492}"

# shellcheck source=/dev/null
source "${BENCH_HERE}/parse.sh"

BENCH_WORK=""
BENCH_PIDS=()

bench_cleanup() {
  for pid in "${BENCH_PIDS[@]:-}"; do
    [[ -n "${pid}" ]] && kill "${pid}" 2>/dev/null
    [[ -n "${pid}" ]] && wait "${pid}" 2>/dev/null
  done
  BENCH_PIDS=()
  # Removed unconditionally: bench_finish moves the directory out first, so
  # anything still here is an incomplete run and must not survive.
  [[ -n "${BENCH_WORK}" ]] && rm -rf "${BENCH_WORK}"
}
trap bench_cleanup EXIT

bench_die() { printf '%s\n' "$*" >&2; exit 1; }

# Opens a run. The environment block is written before anything else, and its
# failure is the caller's failure.
bench_begin() {
  BENCH_WORK="$(mktemp -d)"
  if ! BUILD_DIR="${BUILD_DIR}" "${BENCH_HERE}/environment.sh" \
        > "${BENCH_WORK}/environment.txt" 2> "${BENCH_WORK}/environment.err"; then
    cat "${BENCH_WORK}/environment.err" >&2
    bench_die "refusing to measure anything without a complete environment record"
  fi
  rm -f "${BENCH_WORK}/environment.err"
}

# Closes a run, moving the work into place under a timestamp. Called only when
# everything succeeded.
bench_finish() {
  mkdir -p "${BENCH_RESULTS}"
  local stamp
  stamp="$(date -u +%Y%m%dT%H%M%SZ)"
  local target="${BENCH_RESULTS}/${stamp}"
  local suffix=1
  while [[ -e "${target}" ]]; do target="${BENCH_RESULTS}/${stamp}-${suffix}"; suffix=$((suffix + 1)); done
  mv "${BENCH_WORK}" "${target}"
  BENCH_WORK=""
  printf '%s\n' "${target}"
}

bench_wait_for_port() {
  local port="$1"
  for _ in $(seq 1 100); do
    if redis-cli -p "${port}" ping > /dev/null 2>&1; then return 0; fi
    sleep 0.1
  done
  return 1
}

# A ThreadSanitizer build will not start under an address space randomised with
# 32 bits of entropy, which is the default on Ubuntu 24.04 kernels:
#
#   FATAL: ThreadSanitizer: unexpected memory mapping
#
# CMakeLists.txt already runs the TEST binaries under `setarch -R` for exactly
# this, and these scripts launch the server themselves, so they need the same
# accommodation -- otherwise the benchmarks are unrunnable on one of the three
# builds. The reasoning is in docs/adr/0003-build-and-test-in-a-container.md.
# CI lowers vm.mmap_rnd_bits instead, where this is a harmless no-op.
bench_launcher() {
  local sanitizer=""
  [[ -r "${BUILD_DIR}/CMakeCache.txt" ]] &&
    sanitizer="$(sed -n 's/^SHARDKV_SANITIZER:[^=]*=//p' "${BUILD_DIR}/CMakeCache.txt" | head -1)"
  if [[ "${sanitizer}" == "thread" ]] && command -v setarch > /dev/null 2>&1; then
    printf 'setarch %s -R' "$(uname -m)"
  fi
}

bench_start_shardkv() { # [shards]
  local shards="${1:-${BENCH_SHARDS}}"
  [[ -x "${BUILD_DIR}/shardkv" ]] || bench_die "no ${BUILD_DIR}/shardkv -- build first"
  # Unquoted on purpose: empty on an ordinary build, and two words on a thread
  # build, where it must reach execve as separate arguments.
  # shellcheck disable=SC2046
  $(bench_launcher) "${BUILD_DIR}/shardkv" --port "${BENCH_SHARDKV_PORT}" \
    --shards "${shards}" > /dev/null 2>&1 &
  BENCH_SHARDKV_PID=$!
  BENCH_PIDS+=("${BENCH_SHARDKV_PID}")
  bench_wait_for_port "${BENCH_SHARDKV_PORT}" || bench_die "shardkv did not come up"
}

bench_start_redis() {
  command -v redis-server > /dev/null 2>&1 || bench_die "no redis-server: the control group cannot run"
  # Persistence off on both sides: this measures the request path, and a
  # background save would put one server's fork in the other's numbers.
  redis-server --port "${BENCH_REDIS_PORT}" --save '' --appendonly no \
    > /dev/null 2>&1 &
  BENCH_REDIS_PID=$!
  BENCH_PIDS+=("${BENCH_REDIS_PID}")
  bench_wait_for_port "${BENCH_REDIS_PORT}" || bench_die "redis-server did not come up"
}

# shard_keys is instrumented like every other target, so on a thread build it
# needs the same launcher the server does. The C++ suites get away with calling
# it directly because they are themselves run under setarch and their children
# inherit the personality; a shell script has no such inheritance.
bench_shard_keys() { # args...
  # shellcheck disable=SC2046
  $(bench_launcher) "${BUILD_DIR}/shard_keys" "$@"
}

bench_rss_kb() { # pid
  sed -n 's/^VmRSS:[[:space:]]*\([0-9]*\).*/\1/p' "/proc/$1/status" 2>/dev/null
}

# Runs redis-benchmark and returns its raw output.
#
# BENCH_FIXTURE_OUTPUT replaces the run with the contents of a file. It exists
# for one test -- that a parse failure reaches the caller instead of stopping at
# the parser -- because arranging for a real run to die halfway is not something
# a test can do reliably. It is never set by anything but the test.
bench_run() { # port args...
  local port="$1"; shift
  if [[ -n "${BENCH_FIXTURE_OUTPUT:-}" ]]; then
    cat "${BENCH_FIXTURE_OUTPUT}"
    return 0
  fi
  redis-benchmark -p "${port}" "$@" 2>&1
}

# Every figure in a results file is a number, so that a reader -- and the smoke
# test -- can tell a measurement from a placeholder. Booleans are 0 and 1.
bench_number() { # name value
  printf '%s: %s\n' "$1" "$2"
}
