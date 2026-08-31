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
# Overridable, and the smoke test overrides it.
#
# A smoke run produces a directory that looks exactly like a real measurement --
# same files, same shape, four hundred requests instead of a million -- and
# `bench_smoke` runs on every build, so without this the committed results
# directory fills up with runs that are indistinguishable from the real one
# until you open them. That is a worse failure than an untidy tree: it is a
# results directory a reader cannot trust.
BENCH_RESULTS="${BENCH_RESULTS:-${BENCH_HERE}/results}"

# §8.1's figures by default; smaller values make the scripts exercisable in a
# few seconds, which is how the smoke test runs them. The same shape
# scripts/soak.sh already uses.
BENCH_REQUESTS="${BENCH_REQUESTS:-1000000}"
BENCH_CLIENTS="${BENCH_CLIENTS:-50}"
BENCH_ROUNDS="${BENCH_ROUNDS:-10}"
BENCH_SHARDS="${BENCH_SHARDS:-$(nproc)}"
BENCH_PIPELINE="${BENCH_PIPELINE:-16}"

# The million requests of §8.1 are a THROUGHPUT figure, at fifty connections.
# Two of the four measurements are not that, and inheriting it would make them
# absurd rather than thorough:
#
#   latency runs at ONE connection, where a million requests is hours of
#   waiting to sharpen a percentile that a hundred thousand already resolves.
#
#   the cross-shard sweep runs `rounds * shards` times -- eighty runs at the
#   defaults -- and each is a single connection. A million each is most of a day
#   for a comparison of medians. What it does need is to stay clear of the
#   classification floor of 100 * shards, and twenty thousand is far above it.
#
# Both are overridable, and each script caps its own size at BENCH_REQUESTS, so
# a caller asking for one small size everywhere -- which is what the smoke test
# does -- still gets it.
BENCH_LATENCY_REQUESTS="${BENCH_LATENCY_REQUESTS:-100000}"
BENCH_CROSS_REQUESTS="${BENCH_CROSS_REQUESTS:-20000}"

# How many threads the LOAD GENERATOR gets.
#
# This is not a tuning knob, it is a correction. §8.1 specifies
# `redis-benchmark -c 50` and says nothing about --threads, and redis-benchmark
# is single-threaded unless told otherwise -- so the command as written puts
# fifty connections through one client thread, and that thread saturates before
# an eight-loop server does. Measured: it tops out around 45k requests a second
# against shardkv, which is a fact about redis-benchmark, not about shardkv.
#
# So throughput is measured BOTH ways and both are reported: once exactly as
# §8.1 writes it, and once with the generator given as many threads as the
# server has shards. Neither is the "true" number -- the second takes cores away
# from the server, on a machine that has only eight -- but reporting only the
# first would publish the instrument's ceiling as the server's.
BENCH_GENERATOR_THREADS="${BENCH_GENERATOR_THREADS:-${BENCH_SHARDS}}"

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

  # And wait until the ports actually stop answering.
  #
  # kill() returns as soon as the signal is delivered, not when the process has
  # gone. The next run refuses to start on a port something is already answering
  # on -- deliberately, so it cannot measure a stranger's server -- so leaving
  # before the socket closes makes two runs in a row fail for no reason. Waiting
  # here keeps that check strict without making it flaky.
  local port
  for port in "${BENCH_SHARDKV_PORT}" "${BENCH_REDIS_PORT}"; do
    for _ in $(seq 1 100); do
      redis-cli -p "${port}" ping > /dev/null 2>&1 || break
      sleep 0.1
    done
  done
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

# Nothing may already be answering on a port we are about to claim.
#
# A stale server, a concurrent run, or a forgotten manual session would answer
# PING just as ours does, and the scripts would benchmark it and report the
# numbers as this build's. Checked before starting rather than after, because
# afterwards the two are indistinguishable.
bench_require_free_port() { # port what
  if redis-cli -p "$1" ping > /dev/null 2>&1; then
    bench_die "something is already answering on port $1; refusing to benchmark it as if it were $2"
  fi
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
  bench_require_free_port "${BENCH_SHARDKV_PORT}" shardkv
  # Unquoted on purpose: empty on an ordinary build, and two words on a thread
  # build, where it must reach execve as separate arguments.
  # shellcheck disable=SC2046
  # BENCH_PINNED is what environment.sh records, so it has to be what actually
  # happens. A field that is present and wrong is worse than one that is
  # missing: it reads as a record of the run.
  local pin=()
  [[ "${BENCH_PINNED:-no}" == "yes" ]] && pin=(--pin)
  $(bench_launcher) "${BUILD_DIR}/shardkv" --port "${BENCH_SHARDKV_PORT}" \
    --shards "${shards}" "${pin[@]}" > /dev/null 2>&1 &
  BENCH_SHARDKV_PID=$!
  BENCH_PIDS+=("${BENCH_SHARDKV_PID}")
  bench_wait_for_port "${BENCH_SHARDKV_PORT}" || bench_die "shardkv did not come up"
}

bench_start_redis() {
  command -v redis-server > /dev/null 2>&1 || bench_die "no redis-server: the control group cannot run"
  bench_require_free_port "${BENCH_REDIS_PORT}" redis-server
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
  # The exit status matters and used to be discarded: a run that could not
  # connect returns non-zero and prints one line, and without this the caller
  # went on to parse that line for a throughput figure and reported its absence
  # instead of its cause.
  local text status
  text="$(redis-benchmark -p "${port}" "$@" 2>&1)"
  status=$?
  printf '%s\n' "${text}"
  return "${status}"
}

# What redis-benchmark actually said, when a parser could not find a figure in
# it.
#
# "no throughput summary line in this output" is true and useless: the reason it
# is not there is in the output, and the output is what nobody printed. A run
# that could not connect says so in its first line, and that line is worth more
# than every layer of shell above it guessing.
bench_explain() { # label text
  printf '%s: the generator produced no usable summary. It said:\n' "$1" >&2
  printf '%s\n' "$2" | tr '\r' '\n' | grep -v '^ *$' | tail -8 | sed 's/^/  | /' >&2
}

# Every figure in a results file is a number, so that a reader -- and the smoke
# test -- can tell a measurement from a placeholder. Booleans are 0 and 1.
#
# It refuses anything that is not one. A blank or a stray word reaching a results
# file is the failure this whole step exists to prevent, and the caller that
# produced it is the one that should die, not the reader who finds it later.
bench_number() { # name value
  case "$2" in
    ''|*[!0-9.+-]*) bench_die "refusing to record '$1' as '$2': that is not a number" ;;
  esac
  printf '%s: %s\n' "$1" "$2"
}
