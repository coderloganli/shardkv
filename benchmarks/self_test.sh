#!/usr/bin/env bash
#
# Test cases 10-23 from task.md, run by ctest as `bench_smoke`.
#
# The scripts under benchmarks/ are a deliverable, so they are tested rather than
# hoped over. Two kinds of case live here:
#
#   - pure functions from parse.sh -- the classifier, the sample-count rule, the
#     output parsers -- driven by synthetic inputs and committed fixtures, with
#     no server behind them. The classifier is the riskiest rule in this step and
#     this is where it gets tested, with no kernel in the way.
#
#   - the scripts end to end at smoke size, against a real shardkv and a real
#     redis-server, asserting that a results directory is either complete or
#     absent.
#
# Failure paths are provoked by CONFIGURATION, never by editing a copy of the
# script: pointing BUILD_DIR at a directory with no CMakeCache.txt is a real
# failure of the real thing, where a patched copy would prove something about the
# copy.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$(cd "${HERE}/.." && pwd)/build}"
export BUILD_DIR

FIXTURES="${HERE}/fixtures"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "${SCRATCH}"' EXIT

passed=0
failed=0

ok()   { passed=$((passed + 1)); printf '  ok   %s\n' "$1"; }
bad()  { failed=$((failed + 1)); printf '  FAIL %s\n     %s\n' "$1" "${2:-}"; }

check_eq() { # name expected actual
  if [[ "$2" == "$3" ]]; then ok "$1"; else bad "$1" "expected '$2', got '$3'"; fi
}
check_contains() { # name haystack needle
  if [[ "$2" == *"$3"* ]]; then ok "$1"; else bad "$1" "'$3' not found in output"; fi
}
check_nonzero_exit() { # name status
  if [[ "$2" -ne 0 ]]; then ok "$1"; else bad "$1" "exited 0, expected non-zero"; fi
}
check_zero_exit() { # name status
  if [[ "$2" -eq 0 ]]; then ok "$1"; else bad "$1" "exited $2, expected 0"; fi
}

# shellcheck source=/dev/null
if [[ -f "${HERE}/parse.sh" ]]; then source "${HERE}/parse.sh"; fi

echo "== the classification rule (cases 10-13) =="

# 10 -- the split is at half the request count, not at zero: INFO is itself a
# cross-shard command and inflates the counter being read.
out="$(classify_run 10 20000 4 2>&1)"; st=$?
check_eq "10a  a small delta is a local run" "local" "${out}"
check_zero_exit "10b  and classifying it succeeds" "${st}"

out="$(classify_run 19999 20000 4 2>&1)"
check_eq "10c  a delta near the request count is a remote run" "remote" "${out}"

out="$(classify_run 9999 20000 4 2>&1)"
check_eq "10d  just under half is still local" "local" "${out}"
out="$(classify_run 10000 20000 4 2>&1)"
check_eq "10e  at half it is remote" "remote" "${out}"

# 11 -- below 100 * shards the instrument cost and the signal are the same order,
# so the rule says so instead of guessing.
out="$(classify_run 3 50 4 2>&1)"
check_eq "11a  too few requests to separate the answers" "unknown" "${out}"
out="$(classify_run 3 400 4 2>&1)"
check_eq "11b  at the floor it classifies again" "local" "${out}"

# 12 -- a difference computed from one sample is not a difference.
out="$(penalty_report "1.0 1.1" "2.0 2.1 2.2 2.3" 2>&1)"; st=$?
check_nonzero_exit "12a  two local samples is a refusal" "${st}"
check_contains "12b  and it names the group that was short" "${out}" "local"

out="$(penalty_report "1.0 1.1 1.2" "2.0 2.1 2.2" 2>&1)"; st=$?
check_zero_exit "12c  three and three is enough to report" "${st}"

# 13 -- ten samples on a noisy virtual machine make a median a reader deserves to
# be able to judge, so the spread goes with it.
out="$(penalty_report "1.0 1.1 1.2" "2.0 2.1 2.2" 2>&1)"
for field in local_n local_median local_min local_max remote_n remote_median remote_min remote_max; do
  check_contains "13   the report carries ${field}" "${out}" "${field}"
done

echo "== the environment block (cases 14-16) =="

# 14
env_out="$("${HERE}/environment.sh" 2>&1)"; st=$?
check_zero_exit "14a  environment.sh succeeds" "${st}"
for field in cpu_model cpu_cores kernel compiler compiler_version build_type \
             redis_server_version redis_benchmark_version same_machine pinned \
             shardkv_commit date; do
  value="$(printf '%s\n' "${env_out}" | sed -n "s/^${field}: *//p")"
  if [[ -n "${value}" ]]; then ok "14   ${field} is present and not empty"
  else bad "14   ${field}" "missing or empty"; fi
done

# 15 -- a real failure of the real script, provoked by configuration.
empty="${SCRATCH}/no-cmake-cache"
mkdir -p "${empty}"
out="$(BUILD_DIR="${empty}" "${HERE}/environment.sh" 2>&1)"; st=$?
check_nonzero_exit "15a  no CMakeCache.txt means no environment block" "${st}"
check_contains "15b  and it says what it could not determine" "${out}" "compiler"

# 16 -- a control group on a different version is a different experiment.
out="$(BENCH_REDIS_SERVER_VERSION=7.0.15 BENCH_REDIS_BENCHMARK_VERSION=7.2.0 \
       "${HERE}/environment.sh" 2>&1)"; st=$?
check_nonzero_exit "16a  mismatched redis versions are refused" "${st}"
check_contains "16b  and the two versions are named" "${out}" "7.2.0"

echo "== parsing (cases 17-20) =="

# 17
check_eq "17a  throughput" "49504.95" "$(parse_throughput < "${FIXTURES}/benchmark-get.txt")"
check_eq "17b  p50"        "0.135"    "$(parse_percentile p50 < "${FIXTURES}/benchmark-get.txt")"
check_eq "17c  p95"        "0.231"    "$(parse_percentile p95 < "${FIXTURES}/benchmark-get.txt")"
check_eq "17d  p99"        "0.391"    "$(parse_percentile p99 < "${FIXTURES}/benchmark-get.txt")"

# 18 -- p999 is not in the summary; it comes from the detailed block, as the
# first entry at or above 99.9 percent.
check_eq "18   p999 from the percentile block" "0.735" \
         "$(parse_p999 < "${FIXTURES}/benchmark-get.txt")"

# 19 -- a run that died halfway must not silently yield a blank field.
out="$(parse_p999 < "${FIXTURES}/benchmark-no-percentiles.txt" 2>&1)"; st=$?
check_nonzero_exit "19a  no percentile block is an error" "${st}"
check_eq "19b  and it produces no number" "" "$(parse_p999 < "${FIXTURES}/benchmark-no-percentiles.txt" 2>/dev/null)"

# 20 -- and the error has to reach the caller, not stop at the parser.
out="$(parse_throughput < "${FIXTURES}/benchmark-truncated.txt" 2>&1)"; st=$?
check_nonzero_exit "20a  no summary line is an error" "${st}"

results_before="$(ls "${HERE}/results" 2>/dev/null | wc -l)"
BENCH_FIXTURE_OUTPUT="${FIXTURES}/benchmark-truncated.txt" \
  BENCH_REQUESTS=400 BENCH_CLIENTS=1 "${HERE}/throughput.sh" > /dev/null 2>&1; st=$?
check_nonzero_exit "20b  and the measurement script fails with it" "${st}"
results_after="$(ls "${HERE}/results" 2>/dev/null | wc -l)"
check_eq "20c  leaving no results directory" "${results_before}" "${results_after}"

echo "== the scripts, end to end (cases 21-23) =="

smoke_env=(BENCH_REQUESTS=400 BENCH_CLIENTS=2 BENCH_ROUNDS=1)

# 21 -- complete or absent. "The directory exists" is not the assertion.
env "${smoke_env[@]}" "${HERE}/run-all.sh" > "${SCRATCH}/run-all.log" 2>&1; st=$?
check_zero_exit "21a  run-all.sh at smoke size succeeds" "${st}"

latest="$(ls -1d "${HERE}"/results/*/ 2>/dev/null | sort | tail -1)"
if [[ -n "${latest}" ]]; then ok "21b  it produced a results directory"
else bad "21b  results directory" "none was created"; fi

if [[ -n "${latest}" ]]; then
  if [[ -s "${latest}/environment.txt" ]]; then ok "21c  with a non-empty environment block"
  else bad "21c  environment.txt" "missing or empty"; fi

  for measurement in throughput latency cross_shard memory; do
    raw="${latest}/${measurement}.raw"
    numbers="${latest}/${measurement}.txt"
    if [[ -s "${raw}" ]]; then ok "21d  ${measurement}: raw output is non-empty"
    else bad "21d  ${measurement}" "raw output missing or empty"; fi

    if [[ -s "${numbers}" ]]; then
      offenders="$(sed -n 's/^[a-z_]*: *//p' "${numbers}" \
                   | grep -vE '^-?[0-9]+(\.[0-9]+)?$' | head -3 || true)"
      if [[ -z "${offenders}" ]]; then ok "21e  ${measurement}: every field is a number"
      else bad "21e  ${measurement}" "non-numeric field(s): ${offenders}"; fi
    else
      bad "21e  ${measurement}" "extracted numbers missing or empty"
    fi
  done
fi

# 22 -- the rule this whole step exists to enforce, asserted for each script on
# its own and not only through run-all.sh.
for script in throughput.sh latency.sh cross_shard.sh memory.sh run-all.sh; do
  before="$(ls "${HERE}/results" 2>/dev/null | wc -l)"
  env "${smoke_env[@]}" BUILD_DIR="${empty}" "${HERE}/${script}" > /dev/null 2>&1; st=$?
  after="$(ls "${HERE}/results" 2>/dev/null | wc -l)"
  check_nonzero_exit "22a  ${script} fails when the environment cannot be recorded" "${st}"
  check_eq "22b  ${script} leaves nothing behind" "${before}" "${after}"
done

# 23 -- both servers loaded the same way, and RSS reported for what it is.
if [[ -n "${latest}" && -s "${latest}/memory.txt" ]]; then
  mem="$(cat "${latest}/memory.txt")"
  for field in requests keyspace value_bytes \
               shardkv_baseline_rss_kb shardkv_loaded_rss_kb \
               redis_baseline_rss_kb redis_loaded_rss_kb; do
    check_contains "23   memory.txt records ${field}" "${mem}" "${field}"
  done
else
  bad "23   memory.txt" "not produced, so nothing to check"
fi

echo
echo "passed ${passed}, failed ${failed}"
[[ "${failed}" -eq 0 ]]
