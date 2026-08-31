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

# Everything this file runs writes its results here, not into benchmarks/results.
# A smoke run is shaped exactly like a real measurement and this runs on every
# build; mixing the two would leave a committed results directory a reader has to
# open a file to trust.
export BENCH_RESULTS="${SCRATCH}/results"

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

# bench_number lives in common.sh, which starts servers and installs traps when
# sourced. Probing it in a subshell keeps this file's own process out of that.
bench_number_probe() { # name value
  ( set -uo pipefail
    # shellcheck source=/dev/null
    source "${HERE}/common.sh" > /dev/null 2>&1
    bench_number "$1" "$2" )
}

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
if [[ "${st}" -ne 0 ]]; then
  # Printed, because a bare "it failed" sent me hunting through a CI log for
  # something the script had already said. The first time this fired, the reason
  # was "Permission denied" -- see case 30.
  printf '     environment.sh said:\n'
  printf '       %s\n' "${env_out}"
fi
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

results_before="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
BENCH_FIXTURE_OUTPUT="${FIXTURES}/benchmark-truncated.txt" \
  BENCH_REQUESTS=400 BENCH_CLIENTS=1 "${HERE}/throughput.sh" > /dev/null 2>&1; st=$?
check_nonzero_exit "20b  and the measurement script fails with it" "${st}"
results_after="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
check_eq "20c  leaving no results directory" "${results_before}" "${results_after}"

echo "== two tests in one output (cases 24-26) =="

# 24 -- `-t get,set` runs two tests and prints two summaries. Naming one must
# give that one's figures, and the two must not be the same number.
set_rps="$(parse_throughput SET < "${FIXTURES}/benchmark-get-set.txt")"
get_rps="$(parse_throughput GET < "${FIXTURES}/benchmark-get-set.txt")"
if [[ -n "${set_rps}" && -n "${get_rps}" ]]; then ok "24a  both sections parse"
else bad "24a  sections" "SET='${set_rps}' GET='${get_rps}'"; fi
if [[ "${set_rps}" != "${get_rps}" ]]; then ok "24b  and they are different runs"
else bad "24b  sections" "SET and GET returned the same figure, so one of them is not being read"; fi

set_p999="$(parse_p999 SET < "${FIXTURES}/benchmark-get-set.txt")"
get_p999="$(parse_p999 GET < "${FIXTURES}/benchmark-get-set.txt")"
if [[ -n "${set_p999}" && -n "${get_p999}" ]]; then ok "24c  p999 per section"
else bad "24c  p999" "SET='${set_p999}' GET='${get_p999}'"; fi

# 25 -- THE CASE THAT WOULD HAVE CAUGHT THE PUBLISHED ERROR. Asked for a figure
# without saying which test, on output that holds two, the parser must refuse.
# Returning the first is what put SET numbers under a "GET/SET" heading.
out="$(parse_throughput < "${FIXTURES}/benchmark-get-set.txt" 2>&1)"; st=$?
check_nonzero_exit "25a  an unnamed section is ambiguous, not the first one" "${st}"
check_eq "25b  and it yields no number" "" "$(parse_throughput < "${FIXTURES}/benchmark-get-set.txt" 2>/dev/null)"
out="$(parse_percentile p50 < "${FIXTURES}/benchmark-get-set.txt" 2>&1)"; st=$?
check_nonzero_exit "25c  the same for percentiles" "${st}"

# 26 -- a section that is not there is an error, not an empty answer.
out="$(parse_throughput NOSUCH < "${FIXTURES}/benchmark-get-set.txt" 2>&1)"; st=$?
check_nonzero_exit "26a  an unknown section is refused" "${st}"
# and a single-test output still parses without naming anything
check_eq "26b  one summary needs no section name" "49504.95" \
         "$(parse_throughput < "${FIXTURES}/benchmark-get.txt")"

echo "== figures must be figures (case 27) =="

# 27 -- a blank or a stray word reaching a results file is the failure this whole
# step exists to prevent, so the recorder refuses it rather than the reader
# discovering it later.
out="$(bench_number_probe 'x' '' 2>&1)"; st=$?
check_nonzero_exit "27a  an empty value is refused" "${st}"
out="$(bench_number_probe 'x' 'nan' 2>&1)"; st=$?
check_nonzero_exit "27b  a word is refused" "${st}"
out="$(bench_number_probe 'x' '12.5' 2>&1)"; st=$?
check_zero_exit "27c  a number is accepted" "${st}"

echo "== the scripts, end to end (cases 21-23) =="

smoke_env=(BENCH_REQUESTS=400 BENCH_CLIENTS=2 BENCH_ROUNDS=1)

# 21 -- complete or absent. "The directory exists" is not the assertion.
env "${smoke_env[@]}" "${HERE}/run-all.sh" > "${SCRATCH}/run-all.log" 2>&1; st=$?
check_zero_exit "21a  run-all.sh at smoke size succeeds" "${st}"

latest="$(ls -1d "${BENCH_RESULTS}"/*/ 2>/dev/null | sort | tail -1)"
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
  before="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
  env "${smoke_env[@]}" BUILD_DIR="${empty}" "${HERE}/${script}" > /dev/null 2>&1; st=$?
  after="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
  check_nonzero_exit "22a  ${script} fails when the environment cannot be recorded" "${st}"
  check_eq "22b  ${script} leaves nothing behind" "${before}" "${after}"
done

# 28 -- the aggregate rule, at the point where it is hardest to hold. Each
# measurement script publishes a directory of its own, so a failure in a LATER
# one could strand the earlier ones -- complete directories, indistinguishable
# from a finished run. Provoked by making the last measurement impossible: a
# shard count of one, which cross_shard.sh must refuse.
before="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
env "${smoke_env[@]}" BENCH_SHARDS=1 "${HERE}/run-all.sh" > /dev/null 2>&1; st=$?
after="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
check_nonzero_exit "28a  run-all fails when a later measurement cannot run" "${st}"
check_eq "28b  and the earlier ones are not left behind" "${before}" "${after}"

# 23 -- both servers loaded the same way, and RSS reported for what it is.
if [[ -n "${latest}" && -s "${latest}/memory.txt" ]]; then
  mem="$(cat "${latest}/memory.txt")"
  for field in requests keyspace value_bytes \
               shardkv_baseline_rss_kb shardkv_loaded_rss_kb shardkv_keys \
               redis_baseline_rss_kb redis_loaded_rss_kb redis_keys; do
    check_contains "23a  memory.txt records ${field}" "${mem}" "${field}"
  done

  field_of() { printf '%s\n' "${mem}" | sed -n "s/^$1: *//p"; }

  # Both servers actually took the load, rather than the script recording a
  # baseline twice: DBSIZE answered with a number, and a positive one.
  for label in shardkv redis; do
    keys="$(field_of "${label}_keys")"
    if [[ "${keys}" =~ ^[0-9]+$ ]] && (( keys > 0 )); then
      ok "23b  ${label} actually holds keys (${keys})"
    else
      bad "23b  ${label}_keys" "'${keys}' is not a positive number, so the load is unverified"
    fi
  done

  # And they were loaded identically. A memory comparison between two different
  # datasets would be a comparison of nothing.
  sk="$(field_of shardkv_keys)"; rd="$(field_of redis_keys)"
  if [[ "${sk}" =~ ^[0-9]+$ && "${rd}" =~ ^[0-9]+$ ]]; then
    spread=$(( sk > rd ? sk - rd : rd - sk ))
    allowed=$(( sk / 20 + 5 ))
    if (( spread <= allowed )); then ok "23c  both servers received the same load"
    else bad "23c  load" "shardkv holds ${sk} keys and redis ${rd}: not the same dataset"; fi
  fi
else
  bad "23   memory.txt" "not produced, so nothing to check"
fi

echo "== the scripts are executable (case 30) =="

# 30 -- every one of these is invoked by path, by ctest, by run-all.sh, and by a
# reader following the README. Without the executable bit they fail with
# "Permission denied" and no output at all, which is exactly what happened: this
# checkout has core.filemode=false, so a local `chmod +x` never reaches the
# index and the file is committed 100644. It works locally and fails everywhere
# else -- so the bit is asserted here rather than remembered.
for script in common.sh cross_shard.sh environment.sh latency.sh memory.sh \
              parse.sh run-all.sh self_test.sh throughput.sh; do
  if [[ -x "${HERE}/${script}" ]]; then ok "30   ${script} is executable"
  else bad "30   ${script}" "not executable; git update-index --chmod=+x benchmarks/${script}"; fi
done
if [[ -x "${HERE}/../scripts/soak.sh" ]]; then ok "30   scripts/soak.sh is executable"
else bad "30   scripts/soak.sh" "not executable"; fi

echo "== the environment record matches the run (case 29) =="

# 29 -- a field that is present and WRONG is worse than one that is missing: it
# reads as a record of what happened. BENCH_PINNED is the one that can drift,
# because environment.sh records it and common.sh has to act on it.
if [[ -n "${latest}" ]]; then
  recorded="$(sed -n 's/^pinned: *//p' "${latest}/environment.txt")"
  check_eq "29a  pinned is recorded as the run was made" "no" "${recorded}"
fi
if grep -q -- '--pin' "${HERE}/common.sh"; then
  ok "29b  and BENCH_PINNED can actually reach the server"
else
  bad "29b  pinned" "environment.sh records it but common.sh never passes --pin"
fi

echo
echo "passed ${passed}, failed ${failed}"
[[ "${failed}" -eq 0 ]]
