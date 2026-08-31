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

# The children publish into a staging area of this run's own, not into
# benchmarks/results.
#
# Each script is independently runnable and so publishes a directory of its own.
# Left pointing at the real results directory, a failure in the fourth
# measurement would strand the first three there -- complete directories, and
# therefore indistinguishable from a run that finished, which is exactly the
# thing "complete or nothing" is supposed to rule out. The staging area is inside
# BENCH_WORK, so the EXIT trap removes it along with everything else.
staging="${BENCH_WORK}/parts"
mkdir -p "${staging}"

for measurement in throughput latency cross_shard memory; do
  script="${BENCH_HERE}/${measurement}.sh"
  produced="$(BENCH_RESULTS="${staging}" "${script}")" \
    || bench_die "run-all: ${measurement}.sh failed"
  [[ -d "${produced}" ]] || bench_die "run-all: ${measurement}.sh produced no results directory"

  cp "${produced}/${measurement}.raw" "${BENCH_WORK}/" \
    || bench_die "run-all: ${measurement} left no raw output"
  cp "${produced}/${measurement}.txt" "${BENCH_WORK}/" \
    || bench_die "run-all: ${measurement} left no figures"
done

rm -rf "${staging}"

bench_finish
