#!/usr/bin/env bash
#
# The four measurements, into one results directory.
#
# Each script is runnable on its own; this runs them in order and gathers what
# they produced. If any of them fails, so does this, and the gathered directory
# is not created -- the same rule the individual scripts follow.
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

bench_begin

for measurement in throughput latency cross_shard memory; do
  script="${BENCH_HERE}/${measurement}.sh"
  produced="$("${script}")" || bench_die "run-all: ${measurement}.sh failed"
  [[ -d "${produced}" ]] || bench_die "run-all: ${measurement}.sh produced no results directory"

  cp "${produced}/${measurement}.raw" "${BENCH_WORK}/" \
    || bench_die "run-all: ${measurement} left no raw output"
  cp "${produced}/${measurement}.txt" "${BENCH_WORK}/" \
    || bench_die "run-all: ${measurement} left no figures"
  # The individual run's own directory is redundant once gathered.
  rm -rf "${produced}"
done

bench_finish
