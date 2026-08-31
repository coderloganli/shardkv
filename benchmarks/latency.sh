#!/usr/bin/env bash
#
# The latency distribution at one connection with no pipelining -- the case
# §8.2 predicts shardkv loses, because Redis's single-threaded path is short and
# heavily optimised and there is no parallelism here to make up for it.
#
# Throughput at one connection is not interesting. Latency is: it is what a
# single client actually waits.
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

bench_begin
raw="${BENCH_WORK}/latency.raw"
out="${BENCH_WORK}/latency.txt"

# One connection, so this is sized for resolving a percentile rather than for
# saturating the server. See the note in common.sh.
requests="${BENCH_LATENCY_REQUESTS}"
(( requests > BENCH_REQUESTS )) && requests="${BENCH_REQUESTS}"

bench_start_shardkv
bench_start_redis

record() { # label port
  local label="$1" port="$2"
  printf '\n===== %s (one connection, no pipelining) =====\n' "${label}" >> "${raw}"
  local text
  text="$(bench_run "${port}" -t get,set -n "${requests}" -c 1)"
  printf '%s\n' "${text}" >> "${raw}"

  local field
  for field in p50 p95 p99; do
    local value
    value="$(printf '%s\n' "${text}" | parse_percentile "${field}")" \
      || bench_die "${label}: no latency summary"
    bench_number "${label}_${field}" "${value}" >> "${out}"
  done
  local p999
  p999="$(printf '%s\n' "${text}" | parse_p999)" || bench_die "${label}: no percentile block"
  bench_number "${label}_p999" "${p999}" >> "${out}"
}

bench_number requests "${requests}" >> "${out}"
bench_number clients 1 >> "${out}"

record shardkv "${BENCH_SHARDKV_PORT}"
record redis   "${BENCH_REDIS_PORT}"

bench_finish
