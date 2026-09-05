#!/usr/bin/env bash
#
# A soak run: keep load on the server and watch RSS and the open descriptor
# count for as long as you ask for.
#
# What it is looking for is a slope, not a number. A flat pair of curves says
# the buffers, the connections and the expiry sampler all give back what they
# take; a rising one says something does not, which is the failure mode that
# survives the test suite unnoticed and exhausts the process a week later.
#
# This is not a benchmark and prints no throughput figure. Any number that were
# to leave this script would need its machine, kernel, compiler and optimisation
# level recorded alongside it, as docs/product.md requires -- and a run under an
# arbitrary background load is the wrong place to take one.
#
# Run it by hand, inside the container:
#
#   docker build -t shardkv-dev .
#   docker run --rm -v "$PWD":/src -w /src shardkv-dev scripts/soak.sh
#
# Default duration is short so that the script itself is exercisable; the full
# hour is SOAK_SECONDS=3600. Output is a CSV beside the script's working
# directory, one row per sample.
set -euo pipefail

SOAK_SECONDS="${SOAK_SECONDS:-60}"
SAMPLE_SECONDS="${SAMPLE_SECONDS:-5}"
PORT="${PORT:-6399}"
CLIENTS="${CLIENTS:-50}"
KEYSPACE="${KEYSPACE:-100000}"
BUILD_DIR="${BUILD_DIR:-build}"
OUT="${OUT:-soak.csv}"

if [[ ! -x "${BUILD_DIR}/shardkv" ]]; then
  echo "no ${BUILD_DIR}/shardkv -- build first:" >&2
  echo "  cmake -B ${BUILD_DIR} -G Ninja && cmake --build ${BUILD_DIR}" >&2
  exit 1
fi

"./${BUILD_DIR}/shardkv" --port "${PORT}" &
SERVER_PID=$!

# Kills the load generator's whole process group, not just the subshell: the
# subshell spends its life blocked in redis-benchmark, and killing it alone
# leaves that child connected to a server that is about to go away. `kill %2`
# would be worse still -- job control is not reliable in a non-interactive
# shell.
LOAD_PID=""
cleanup() {
  if [[ -n "${LOAD_PID}" ]]; then
    kill -- "-${LOAD_PID}" 2>/dev/null || kill "${LOAD_PID}" 2>/dev/null || true
    wait "${LOAD_PID}" 2>/dev/null || true
  fi
  kill "${SERVER_PID}" 2>/dev/null || true
  wait "${SERVER_PID}" 2>/dev/null || true
}
trap cleanup EXIT

sleep 1
if ! kill -0 "${SERVER_PID}" 2>/dev/null; then
  echo "the server exited immediately" >&2
  exit 1
fi

# Load, restarted as it finishes, so the server is busy for the whole run rather
# than for one benchmark's worth of it.
#
# -t set,get,incr rather than the full suite: those are the single-key commands
# this architecture is built for, and a soak wants steady pressure rather than
# coverage. A share of the keys carry a TTL, so the expiry sampler has work to
# do -- a soak that never expires anything would not exercise the thing most
# likely to hold memory.
set -m  # the load generator gets a process group of its own, so it can be killed
(
  while true; do
    redis-benchmark -p "${PORT}" -c "${CLIENTS}" -n 200000 -r "${KEYSPACE}" \
      -t set,get,incr -q > /dev/null 2>&1 || true
    redis-cli -p "${PORT}" set soak:ttl v EX 2 > /dev/null 2>&1 || true
  done
) &
LOAD_PID=$!
set +m

echo "elapsed_seconds,rss_kb,open_fds,connections,keys,short_writes,read_pauses,accept_failures" \
  | tee "${OUT}"

started=$(date +%s)
while true; do
  now=$(date +%s)
  elapsed=$(( now - started ))
  [[ "${elapsed}" -ge "${SOAK_SECONDS}" ]] && break

  rss=$(awk '/^VmRSS:/ {print $2}' "/proc/${SERVER_PID}/status" 2>/dev/null || echo 0)
  fds=$(ls "/proc/${SERVER_PID}/fd" 2>/dev/null | wc -l)

  info=$(redis-cli -p "${PORT}" info 2>/dev/null || true)
  field() { echo "${info}" | grep -m1 "^$1:" | cut -d: -f2 | tr -d '\r' || true; }

  # Summed across loops: which loop a connection landed on is the kernel's
  # business, so a per-loop number would move about between runs for reasons
  # that have nothing to do with a leak.
  sum_field() {
    echo "${info}" | grep "^loop[0-9]*_$1:" | cut -d: -f2 | tr -d '\r' \
      | awk '{ total += $1 } END { print total + 0 }'
  }
  sum_shard_keys() {
    echo "${info}" | grep "^shard[0-9]*_keys:" | cut -d: -f2 | tr -d '\r' \
      | awk '{ total += $1 } END { print total + 0 }'
  }

  echo "${elapsed},${rss:-0},${fds:-0},$(sum_field connections),$(sum_shard_keys),$(sum_field short_writes),$(sum_field read_pauses),$(sum_field accept_failures)" \
    | tee -a "${OUT}"

  sleep "${SAMPLE_SECONDS}"
done

echo >&2
echo "wrote ${OUT}. What to look at:" >&2
echo "  rss_kb and open_fds must be flat after the first sample or two." >&2
echo "  keys must stop rising: the expiry sampler is what makes it settle." >&2
echo "  a rising open_fds is a descriptor leak and nothing else." >&2
