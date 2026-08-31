#!/usr/bin/env bash
#
# Throughput, with and without pipelining, for shardkv and for redis-server on
# the same machine under the same load.
#
# §8.1's figures: -t get,set -n 1000000 -c 50, and the same again with -P 16.
# The absolute numbers are depressed by the load generator competing with the
# server for the same cores -- see same_machine in the environment block. The
# control group runs under exactly that competition, which is why the difference
# is the thing worth reading.
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

bench_begin
raw="${BENCH_WORK}/throughput.raw"
out="${BENCH_WORK}/throughput.txt"

bench_start_shardkv
bench_start_redis

record() { # label port pipeline
  local label="$1" port="$2" pipeline="$3"
  local args=(-t get,set -n "${BENCH_REQUESTS}" -c "${BENCH_CLIENTS}")
  [[ "${pipeline}" -gt 1 ]] && args+=(-P "${pipeline}")

  printf '\n===== %s (pipeline %s) =====\n' "${label}" "${pipeline}" >> "${raw}"
  local text
  text="$(bench_run "${port}" "${args[@]}")"
  printf '%s\n' "${text}" >> "${raw}"

  local rps p50 p99 p999
  rps="$(printf '%s\n' "${text}" | parse_throughput)"  || bench_die "${label}: no throughput in the output"
  p50="$(printf '%s\n' "${text}" | parse_percentile p50)" || bench_die "${label}: no latency summary"
  p99="$(printf '%s\n' "${text}" | parse_percentile p99)" || bench_die "${label}: no latency summary"
  p999="$(printf '%s\n' "${text}" | parse_p999)"       || bench_die "${label}: no percentile block"

  bench_number "${label}_p${pipeline}_rps"  "${rps}"  >> "${out}"
  bench_number "${label}_p${pipeline}_p50"  "${p50}"  >> "${out}"
  bench_number "${label}_p${pipeline}_p99"  "${p99}"  >> "${out}"
  bench_number "${label}_p${pipeline}_p999" "${p999}" >> "${out}"
}

bench_number requests "${BENCH_REQUESTS}" >> "${out}"
bench_number clients  "${BENCH_CLIENTS}"  >> "${out}"
bench_number shards   "${BENCH_SHARDS}"   >> "${out}"

record shardkv "${BENCH_SHARDKV_PORT}" 1
record redis   "${BENCH_REDIS_PORT}"   1
record shardkv "${BENCH_SHARDKV_PORT}" "${BENCH_PIPELINE}"
record redis   "${BENCH_REDIS_PORT}"   "${BENCH_PIPELINE}"

bench_finish
