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

# label port pipeline generator-threads
#
# The last argument is why this measurement is taken twice. See the note on
# BENCH_GENERATOR_THREADS in common.sh: `redis-benchmark -c 50` is fifty
# connections through ONE client thread unless --threads says otherwise, and one
# client thread saturates before an eight-loop server does.
record() {
  local label="$1" port="$2" pipeline="$3" threads="$4"
  local args=(-t get,set -n "${BENCH_REQUESTS}" -c "${BENCH_CLIENTS}")
  [[ "${pipeline}" -gt 1 ]] && args+=(-P "${pipeline}")
  [[ "${threads}" -gt 1 ]] && args+=(--threads "${threads}")

  local suffix="p${pipeline}"
  [[ "${threads}" -gt 1 ]] && suffix="${suffix}_t${threads}"

  printf '\n===== %s (pipeline %s, generator threads %s) =====\n' \
    "${label}" "${pipeline}" "${threads}" >> "${raw}"
  local text
  text="$(bench_run "${port}" "${args[@]}")"
  printf '%s\n' "${text}" >> "${raw}"

  # SET and GET are two tests and two summaries. Naming them is not tidiness:
  # taking whichever summary came first silently reported SET figures under a
  # "GET/SET" heading once already.
  local op rps p50 p99 p999
  for op in SET GET; do
    local key="${label}_${suffix}_$(printf '%s' "${op}" | tr 'A-Z' 'a-z')"
    rps="$(printf '%s\n' "${text}" | parse_throughput "${op}")"  || bench_die "${label} ${op}: no throughput in the output"
    p50="$(printf '%s\n' "${text}" | parse_percentile p50 "${op}")" || bench_die "${label} ${op}: no latency summary"
    p99="$(printf '%s\n' "${text}" | parse_percentile p99 "${op}")" || bench_die "${label} ${op}: no latency summary"
    p999="$(printf '%s\n' "${text}" | parse_p999 "${op}")"       || bench_die "${label} ${op}: no percentile block"

    bench_number "${key}_rps"  "${rps}"  >> "${out}"
    bench_number "${key}_p50"  "${p50}"  >> "${out}"
    bench_number "${key}_p99"  "${p99}"  >> "${out}"
    bench_number "${key}_p999" "${p999}" >> "${out}"
  done
}

bench_number requests "${BENCH_REQUESTS}" >> "${out}"
bench_number clients  "${BENCH_CLIENTS}"  >> "${out}"
bench_number shards   "${BENCH_SHARDS}"   >> "${out}"
bench_number generator_threads "${BENCH_GENERATOR_THREADS}" >> "${out}"

# Exactly as §8.1 writes it: one generator thread.
record shardkv "${BENCH_SHARDKV_PORT}" 1 1
record redis   "${BENCH_REDIS_PORT}"   1 1
record shardkv "${BENCH_SHARDKV_PORT}" "${BENCH_PIPELINE}" 1
record redis   "${BENCH_REDIS_PORT}"   "${BENCH_PIPELINE}" 1

# And again with the generator able to keep up.
if (( BENCH_GENERATOR_THREADS > 1 )); then
  record shardkv "${BENCH_SHARDKV_PORT}" 1 "${BENCH_GENERATOR_THREADS}"
  record redis   "${BENCH_REDIS_PORT}"   1 "${BENCH_GENERATOR_THREADS}"
fi

bench_finish
