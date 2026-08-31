#!/usr/bin/env bash
#
# The cross-shard penalty: what an extra hop between threads costs a request.
#
# The measurement cannot be arranged, so it is observed. `redis-benchmark` cannot
# choose keys by shard -- shard_keys solves that half -- and which loop accepts a
# connection is the kernel's business, which nothing here can or should assume.
# So each run is classified after the fact by whether cross_shard_requests moved:
# near zero and the connection's loop happened to own the shard the keys belong
# to, near the request count and every request crossed a thread.
#
# See docs/adr/0015-the-cross-shard-penalty-is-observed-not-arranged.md
#
# This script starts its OWN server on its own port and is the only client on it.
# The counter rises for every cross-shard message any client causes, so a second
# client would be measured as part of this one.
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

bench_begin
raw="${BENCH_WORK}/cross_shard.raw"
out="${BENCH_WORK}/cross_shard.txt"

shards="${BENCH_SHARDS}"
(( shards < 2 )) && bench_die "the cross-shard penalty needs at least two shards"

# `rounds * shards` runs of this, so it is sized for the sweep rather than for
# throughput -- but well above the classification floor of 100 * shards. See the
# note in common.sh.
requests="${BENCH_CROSS_REQUESTS}"
(( requests > BENCH_REQUESTS )) && requests="${BENCH_REQUESTS}"

bench_start_shardkv "${shards}"
port="${BENCH_SHARDKV_PORT}"

# Summed across loops: which loop a connection landed on is not this script's to
# decide, so a per-loop number would move about for reasons that have nothing to
# do with the penalty.
counter() {
  redis-cli -p "${port}" info 2>/dev/null \
    | sed -n 's/^loop[0-9]*_cross_shard_requests:\([0-9]*\).*/\1/p' \
    | awk '{ total += $1 } END { print total + 0 }'
}

locals=""
remotes=""
unknowns=0

printf 'shards=%s requests=%s rounds=%s\n' "${shards}" "${requests}" "${BENCH_ROUNDS}" >> "${raw}"

for round in $(seq 1 "${BENCH_ROUNDS}"); do
  for shard in $(seq 0 $((shards - 1))); do
    key="$(bench_shard_keys --shards "${shards}" --shard "${shard}" --count 1)" \
      || bench_die "shard_keys failed for shard ${shard} of ${shards}"
    [[ -n "${key}" ]] || bench_die "shard_keys produced no key for shard ${shard}"

    redis-cli -p "${port}" set "${key}" v > /dev/null 2>&1

    before="$(counter)"
    text="$(bench_run "${port}" -n "${requests}" -c 1 -k 1 GET "${key}")"
    after="$(counter)"
    delta=$((after - before))

    latency="$(printf '%s\n' "${text}" | parse_percentile p50)" \
      || bench_die "round ${round} shard ${shard}: no latency summary"

    verdict="$(classify_run "${delta}" "${requests}" "${shards}")"
    printf 'round=%s shard=%s delta=%s p50=%s verdict=%s\n' \
      "${round}" "${shard}" "${delta}" "${latency}" "${verdict}" >> "${raw}"

    case "${verdict}" in
      local)  locals="${locals} ${latency}" ;;
      remote) remotes="${remotes} ${latency}" ;;
      *)      unknowns=$((unknowns + 1)) ;;
    esac
  done
done

# The single-key sweep above is the case that matters -- at N shards roughly
# (N-1)/N of single-key traffic takes the remote path. The multi-key pair below
# needs no locality: four keys on one shard cost at most one message, four keys
# on four shards cost up to four.
mget_latency() { # label keys...
  local label="$1"; shift
  printf '\n===== MGET %s =====\n' "${label}" >> "${raw}"
  local before after text
  before="$(counter)"
  text="$(bench_run "${port}" -n "${requests}" -c 1 -k 1 MGET "$@")"
  after="$(counter)"
  printf '%s\n' "${text}" >> "${raw}"
  printf 'mget_%s_delta=%s\n' "${label}" "$((after - before))" >> "${raw}"

  local p50
  p50="$(printf '%s\n' "${text}" | parse_percentile p50)" \
    || bench_die "MGET ${label}: no latency summary"
  bench_number "mget_${label}_p50" "${p50}" >> "${out}"
  bench_number "mget_${label}_messages" "$((after - before))" >> "${out}"
}

readarray -t one_shard < <(bench_shard_keys --shards "${shards}" --shard 0 --count 4)
spread=()
for shard in 0 1 2 3; do
  spread+=("$(bench_shard_keys --shards "${shards}" --shard $((shard % shards)) --count 1)")
done
for key in "${one_shard[@]}" "${spread[@]}"; do
  redis-cli -p "${port}" set "${key}" v > /dev/null 2>&1
done

bench_number requests "${requests}" >> "${out}"
bench_number rounds   "${BENCH_ROUNDS}"   >> "${out}"
bench_number shards   "${shards}"         >> "${out}"
bench_number unclassified_runs "${unknowns}" >> "${out}"

mget_latency one_shard "${one_shard[@]}"
mget_latency four_shards "${spread[@]}"

# The report refuses when either group is too thin, and that refusal is a result
# rather than an error: at smoke sizes classify_run answers "unknown" for every
# run by design, so there is nothing to report and the script must still succeed.
if report="$(penalty_report "${locals}" "${remotes}" 2>"${BENCH_WORK}/penalty.err")"; then
  printf '%s\n' "${report}" >> "${out}"
  bench_number penalty_reported 1 >> "${out}"
else
  cat "${BENCH_WORK}/penalty.err" >> "${raw}"
  printf 'penalty not reported; see cross_shard.raw\n' >&2
  bench_number penalty_reported 0 >> "${out}"
fi
rm -f "${BENCH_WORK}/penalty.err"

bench_finish
