#!/usr/bin/env bash
#
# Resident memory after a million keys, for both servers on the same dataset.
#
# What this number is: WHOLE-PROCESS resident memory -- the table, the
# allocator's retention and fragmentation, the per-thread stacks and buffers, and
# the process baseline, all together. It is NOT an isolated per-key overhead, and
# calling it one would claim a decomposition this does not perform: shardkv runs
# a thread and a buffer per shard where Redis runs one of each, and the two
# allocators behave differently.
#
# So the baseline is recorded as well as the loaded figure, for both servers. The
# difference is the closer thing to "what a million keys cost", and even it is
# not clean.
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

bench_begin
raw="${BENCH_WORK}/memory.raw"
out="${BENCH_WORK}/memory.txt"

BENCH_VALUE_BYTES="${BENCH_VALUE_BYTES:-3}"

bench_start_shardkv
bench_start_redis

# Identical load on both: same request count, same keyspace, same value size.
# Anything else would be measuring two different datasets and reporting the
# difference as an architectural one.
load() { # label port pid
  local label="$1" port="$2" pid="$3"
  local baseline loaded
  baseline="$(bench_rss_kb "${pid}")"
  [[ -n "${baseline}" ]] || bench_die "${label}: could not read RSS for pid ${pid}"

  printf '\n===== %s =====\n' "${label}" >> "${raw}"
  local text
  # Checked, not assumed: a load that failed leaves the RSS reading meaning
  # nothing, and the script would go on to report it as "after loading".
  text="$(bench_run "${port}" -t set -n "${BENCH_REQUESTS}" -r "${BENCH_REQUESTS}" \
    -d "${BENCH_VALUE_BYTES}" -c "${BENCH_CLIENTS}")" \
    || bench_die "${label}: the load generator failed"
  printf '%s\n' "${text}" >> "${raw}"
  printf '%s\n' "${text}" | parse_throughput SET > /dev/null \
    || bench_die "${label}: the load did not complete"

  loaded="$(bench_rss_kb "${pid}")"
  [[ -n "${loaded}" ]] || bench_die "${label}: the server died during loading"

  bench_number "${label}_baseline_rss_kb" "${baseline}" >> "${out}"
  bench_number "${label}_loaded_rss_kb"   "${loaded}"   >> "${out}"
  bench_number "${label}_growth_rss_kb"   "$((loaded - baseline))" >> "${out}"
  local keys
  keys="$(redis-cli -p "${port}" dbsize 2>/dev/null | tr -d '\r')"
  [[ -n "${keys}" ]] || bench_die "${label}: DBSIZE did not answer, so the load cannot be verified"
  bench_number "${label}_keys" "${keys}" >> "${out}"
}

bench_number requests    "${BENCH_REQUESTS}"     >> "${out}"
bench_number keyspace    "${BENCH_REQUESTS}"     >> "${out}"
bench_number value_bytes "${BENCH_VALUE_BYTES}"  >> "${out}"
bench_number shards      "${BENCH_SHARDS}"       >> "${out}"

load shardkv "${BENCH_SHARDKV_PORT}" "${BENCH_SHARDKV_PID}"
load redis   "${BENCH_REDIS_PORT}"   "${BENCH_REDIS_PID}"

bench_finish
