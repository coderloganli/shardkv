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

# This run asserts its own CPU model, and says so in the value.
#
# Not a workaround: it is the mechanism working as
# docs/adr/0018-an-environment-field-may-be-asserted-never-guessed.md describes.
# A smoke run publishes no figures -- it exercises the scripts -- so what it
# records about the machine is beside the point, while on a host that cannot
# name its own CPU (an aarch64 Linux guest on macOS) the scripts
# would otherwise refuse to start and this suite could never be green there.
#
# The cases below that test DETECTION pass BENCH_CPU_MODEL= to switch this off,
# an empty assertion being no assertion at all.
export BENCH_CPU_MODEL="${BENCH_CPU_MODEL:-smoke test, not a measured machine}"

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
# What common.sh's floor is set to, read the way a caller would see it.
BENCH_MIN_REQUESTS_SEEN="$(
  ( set -uo pipefail
    # shellcheck source=/dev/null
    source "${HERE}/common.sh" > /dev/null 2>&1
    printf '%s' "${BENCH_MIN_REQUESTS}" )
)"
BENCH_MIN_SAMPLES_SEEN="$(
  ( set -uo pipefail
    # shellcheck source=/dev/null
    source "${HERE}/common.sh" > /dev/null 2>&1
    printf '%s' "${BENCH_MIN_SAMPLES}" )
)"

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
if [[ "${st}" -ne 0 ]]; then
  # Same reason environment.sh's message is printed above: a bare "it failed"
  # sends whoever reads the CI log hunting for something the script has already
  # said. The scripts explain themselves; the test has to stop eating it.
  printf '     run-all.sh said:\n'
  sed 's/^/       /' "${SCRATCH}/run-all.log" | tail -25
fi
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

echo "== a run too fast to time (case 31) =="

# 31 -- redis-benchmark divides the request count by the elapsed time, which it
# measures to hundredths of a second, so a short enough run prints
# "inf requests per second". That is not a very large throughput, it is the
# absence of one, and it reached CI as the misleading "no throughput summary
# line in this output" -- a line that was in fact right there.
out="$(parse_throughput < "${FIXTURES}/benchmark-untimed.txt" 2>&1)"; st=$?
check_nonzero_exit "31a  an untimed run is refused" "${st}"
check_contains "31b  and the reason is named" "${out}" "too fast to be timed"
check_eq "31c  and no figure is produced" "" \
         "$(parse_throughput < "${FIXTURES}/benchmark-untimed.txt" 2>/dev/null)"

# and the floor that stops the scripts provoking it
if (( BENCH_MIN_REQUESTS_SEEN >= 1000 )); then ok "31d  the scripts keep a floor under the request count"
else bad "31d  request floor" "BENCH_MIN_REQUESTS is ${BENCH_MIN_REQUESTS_SEEN}"; fi

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

# ---------------------------------------------------------------------------
# The profiling step's cases. Numbered A1-A9 and B10-B24 to keep them apart from
# the cases above, which are numbered by an earlier task's document.
# ---------------------------------------------------------------------------

echo "== naming the CPU, or refusing to (cases A1-A9) =="

# The detection chain is /proc/cpuinfo, then lscpu, then an assertion from the
# operator. Provoked by configuration, never by editing a copy of the script:
# BENCH_CPU_PROC and BENCH_CPU_LSCPU point the two detectors at files, so a
# machine that HAS a model name can still exercise the path of one that does not.
no_model="${SCRATCH}/cpuinfo-without-model"
printf 'processor\t: 0\nBogoMIPS\t: 48.00\nCPU implementer\t: 0x61\n' > "${no_model}"
with_model="${SCRATCH}/cpuinfo-with-model"
printf 'processor\t: 0\nmodel name\t: A Very Real CPU\n' > "${with_model}"
lscpu_real="${SCRATCH}/lscpu-real"
printf 'Architecture:  aarch64\nModel name:    Some Named Part\n' > "${lscpu_real}"
lscpu_dash="${SCRATCH}/lscpu-dash"
printf 'Architecture:  aarch64\nVendor ID:     Apple\nModel name:    -\n' > "${lscpu_dash}"

cpu_field() { sed -n 's/^cpu_model: *//p'; }

# A1 -- the ordinary x86 path is unchanged.
out="$(BENCH_CPU_MODEL= BENCH_CPU_PROC="${with_model}" "${HERE}/environment.sh" 2>"${SCRATCH}/a1.err")"; st=$?
check_zero_exit "A1a  a detected model is recorded" "${st}"
check_eq "A1b  and it is the detected value" "A Very Real CPU" "$(printf '%s\n' "${out}" | cpu_field)"
if grep -qi 'assert' "${SCRATCH}/a1.err"; then
  bad "A1c  a detected model is not marked asserted" "warned anyway"
else ok "A1c  a detected model is not marked asserted"; fi

# A2 -- no model line in /proc/cpuinfo, but lscpu knows.
out="$(BENCH_CPU_MODEL= BENCH_CPU_PROC="${no_model}" BENCH_CPU_LSCPU="${lscpu_real}" \
       "${HERE}/environment.sh" 2>/dev/null)"; st=$?
check_zero_exit "A2a  lscpu answers when cpuinfo does not" "${st}"
check_eq "A2b  and its value is recorded" "Some Named Part" "$(printf '%s\n' "${out}" | cpu_field)"

# A3 -- lscpu's placeholder is not an answer. This is the case on the machine
# this was written on: the hypervisor tells the guest nothing.
out="$(BENCH_CPU_MODEL= BENCH_CPU_PROC="${no_model}" BENCH_CPU_LSCPU="${lscpu_dash}" \
       "${HERE}/environment.sh" 2>&1)"; st=$?
check_nonzero_exit "A3   a dash from lscpu is not a model name" "${st}"

# A4 -- and then the operator may say what it is.
out="$(BENCH_CPU_PROC="${no_model}" BENCH_CPU_LSCPU="${lscpu_dash}" \
       BENCH_CPU_MODEL="Apple M5 Pro" "${HERE}/environment.sh" 2>"${SCRATCH}/a4.err")"; st=$?
check_zero_exit "A4a  an asserted model is accepted" "${st}"
check_eq "A4b  and it is marked as asserted" "Apple M5 Pro (asserted)" \
         "$(printf '%s\n' "${out}" | cpu_field)"
if [[ -s "${SCRATCH}/a4.err" ]]; then ok "A4c  and it warns on standard error"
else bad "A4c  asserted model" "no warning was printed"; fi

# A5 -- with nothing to detect and nothing asserted, it still refuses, and it
# says what the person who hit it can do about it.
out="$(BENCH_CPU_MODEL= BENCH_CPU_PROC="${no_model}" BENCH_CPU_LSCPU="${lscpu_dash}" \
       "${HERE}/environment.sh" 2>&1)"; st=$?
check_nonzero_exit "A5a  an unnameable CPU is a refusal" "${st}"
check_contains "A5b  and it names the field" "${out}" "cpu_model"
check_contains "A5c  and it offers the way out" "${out}" "BENCH_CPU_MODEL"

# A6 -- an empty assertion is not an assertion.
out="$(BENCH_CPU_PROC="${no_model}" BENCH_CPU_LSCPU="${lscpu_dash}" \
       BENCH_CPU_MODEL="" "${HERE}/environment.sh" 2>&1)"; st=$?
check_nonzero_exit "A6   an empty assertion is refused like none at all" "${st}"

# A7 -- assertion wins over detection, as it already does for the commit hash.
out="$(BENCH_CPU_PROC="${with_model}" BENCH_CPU_MODEL="What The Operator Says" \
       "${HERE}/environment.sh" 2>"${SCRATCH}/a7.err")"; st=$?
check_zero_exit "A7a  an assertion is taken over a detection" "${st}"
check_eq "A7b  and the operator's value is what lands" "What The Operator Says (asserted)" \
         "$(printf '%s\n' "${out}" | cpu_field)"

# A8 -- the block is `name: value` lines, so a value carrying a newline can
# forge a field. Refused, and refused before anything is printed: a recorder
# that emits half a block and then fails has still put half a block somewhere.
out="$(BENCH_CPU_PROC="${no_model}" BENCH_CPU_LSCPU="${lscpu_dash}" \
       BENCH_CPU_MODEL="Real CPU
cpu_cores: 999" "${HERE}/environment.sh" 2>/dev/null)"; st=$?
check_nonzero_exit "A8a  an assertion that could forge a field is refused" "${st}"
check_eq "A8b  and nothing at all was printed" "" "${out}"

# A9 -- the caller's contract, which is the one that actually protects a reader:
# an unnameable CPU means no results directory, not a partial one.
before="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
BENCH_CPU_MODEL= BENCH_CPU_PROC="${no_model}" BENCH_CPU_LSCPU="${lscpu_dash}" \
  BENCH_REQUESTS=400 BENCH_CLIENTS=1 "${HERE}/latency.sh" > /dev/null 2>&1; st=$?
after="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
check_nonzero_exit "A9a  a measurement refuses when the CPU cannot be named" "${st}"
check_eq "A9b  and publishes nothing" "${before}" "${after}"

echo "== reading a profile (cases B18-B23) =="

# The fixtures are a REAL recording, captured from this server under load, not
# written by hand. A parser that has only ever seen invented input can pass every
# case here and find nothing in the real thing -- which is exactly what happened
# once already, when redis-benchmark turned out to draw its section headings at
# the END of a line.

# B18 -- the ranked symbols, and the event they were sampled on. The event name
# matters as much as the numbers: `:u` is what says this profile saw user space
# only, and every claim made from it depends on that being true.
check_eq "B18a  the event is named" "cpu-clock:u" \
         "$(parse_profile_event < "${FIXTURES}/perf-report.txt")"
check_eq "B18b  the hottest symbol" "recv" \
         "$(parse_profile_top 1 < "${FIXTURES}/perf-report.txt" | awk '{print $2}')"
check_eq "B18c  and its share" "20.58" \
         "$(parse_profile_top 1 < "${FIXTURES}/perf-report.txt" | awk '{print $1}')"
check_eq "B18d  the ranking is as deep as asked for" "5" \
         "$(parse_profile_top 5 < "${FIXTURES}/perf-report.txt" | wc -l | tr -d ' ')"

# B19 -- user-mode CPU nanoseconds, which is the denominator every percentage
# above is a share of. Without it the table is a set of shares of nothing.
check_eq "B19  the event count is the denominator" "3643643640" \
         "$(parse_profile_event_count < "${FIXTURES}/perf-report.txt")"

# B20 -- the exact sample count is NOT in the report: its header abbreviates
# ("Samples: 3K"). perf record prints the exact figure on standard error, so
# that is where it comes from, and this fixture is that line.
check_eq "B20  the sample count comes from the recording" "727" \
         "$(parse_profile_samples < "${FIXTURES}/perf-record-stderr.txt")"

# B21 -- a recording that captured nothing prints no sample count at all. That
# is a refusal, not a zero.
printf '[ perf record: Woken up 1 times to write data ]\n[ perf record: Captured and wrote 0.002 MB /tmp/x.data ]\n' \
  > "${SCRATCH}/no-samples.txt"
out="$(parse_profile_samples < "${SCRATCH}/no-samples.txt" 2>&1)"; st=$?
check_nonzero_exit "B21  a recording with no samples is an error, not a zero" "${st}"

# B22 -- an output with no ranked lines at all.
printf '# Samples: 0 of event %s\n#\n' "'cpu-clock:u'" > "${SCRATCH}/no-rows.txt"
out="$(parse_profile_top 5 < "${SCRATCH}/no-rows.txt" 2>&1)"; st=$?
check_nonzero_exit "B22  a report with no ranked symbols is an error" "${st}"

# B23 -- and one whose header is gone, so the event cannot be confirmed. An
# unconfirmed event means an unconfirmed scope, which means no claims.
printf '    20.58%%  shardkv  libc.so.6  [.] recv\n' > "${SCRATCH}/no-header.txt"
out="$(parse_profile_event < "${SCRATCH}/no-header.txt" 2>&1)"; st=$?
check_nonzero_exit "B23  a report with no event header is an error" "${st}"

# B24 -- unresolved addresses. A profile whose symbols did not resolve fails
# quietly: every number is there, and none of them has a name.
pct="$(parse_profile_unresolved < "${FIXTURES}/perf-report.txt")"
if [[ -n "${pct}" ]] && awk -v p="${pct}" 'BEGIN{exit !(p > 0 && p < 50)}'; then
  ok "B24  the unresolved share is measured"
else
  bad "B24  unresolved share" "got '${pct}', expected a percentage between 0 and 50"
fi

echo "== the profiling script (cases B10-B17) =="

# B10 -- no perf, no profile. Provoked by configuration: PERF names the binary.
out="$(BENCH_PERF="${SCRATCH}/there-is-no-perf-here" "${HERE}/profile.sh" 2>&1)"; st=$?
check_nonzero_exit "B10a  no perf is a refusal" "${st}"
check_contains "B10b  and it says what is missing" "${out}" "perf"

# B11 -- perf present but not permitted. The message must carry what perf itself
# said, and must NOT assert a single cause: the same failure comes from the
# container's syscall filter, from perf_event_paranoid, and from a kernel
# without the tooling. Naming one of those as THE reason sends people to fix
# the wrong thing.
fake_perf="${SCRATCH}/perf-denied"
printf '#!/bin/sh\necho "No permission to enable task-clock event." >&2\nexit 255\n' > "${fake_perf}"
chmod +x "${fake_perf}"
out="$(BENCH_PERF="${fake_perf}" "${HERE}/profile.sh" 2>&1)"; st=$?
check_nonzero_exit "B11a  a denied perf is a refusal" "${st}"
check_contains "B11b  and it quotes what perf said" "${out}" "No permission to enable"
check_contains "B11c  and it offers the candidate causes" "${out}" "perf_event_paranoid"

# B12 -- the same environment contract every other measurement follows.
before="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
empty2="${SCRATCH}/no-cache-for-profile"
mkdir -p "${empty2}"
BUILD_DIR="${empty2}" "${HERE}/profile.sh" > /dev/null 2>&1; st=$?
after="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
check_nonzero_exit "B12a  no environment record, no profile" "${st}"
check_eq "B12b  and nothing is published" "${before}" "${after}"

# B13 -- a real run, if this machine can take one. When it cannot, the assertion
# is that the script REFUSES rather than that it produces something: both
# outcomes are definite, and neither is a skip.
before="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
produced="$(BENCH_PROFILE_SECONDS=3 BENCH_REQUESTS=20000 BENCH_CLIENTS=8 \
            "${HERE}/profile.sh" 2>"${SCRATCH}/b13.err")"; st=$?
after="$(ls "${BENCH_RESULTS}" 2>/dev/null | wc -l)"
if [[ "${st}" -eq 0 ]]; then
  ok "B13a  a profile was recorded on this machine"
  for f in environment.txt profile.raw profile.txt; do
    if [[ -s "${produced}/${f}" ]]; then ok "B13   ${f} is present and not empty"
    else bad "B13   ${f}" "missing or empty"; fi
  done
  # B14 -- units in the names, because a bare number invites the reader to
  # supply their own.
  for field in profile_samples user_cpu_seconds kernel_cpu_seconds; do
    check_contains "B14   ${field} is recorded" "$(cat "${produced}/profile.txt")" "${field}"
  done
  # B15 -- the denominator is not optional.
  ucpu="$(sed -n 's/^user_cpu_seconds: *//p' "${produced}/profile.txt")"
  kcpu="$(sed -n 's/^kernel_cpu_seconds: *//p' "${produced}/profile.txt")"
  if [[ -n "${ucpu}" && -n "${kcpu}" ]]; then ok "B15  both halves of the CPU time are recorded"
  else bad "B15  cpu time" "user='${ucpu}' kernel='${kcpu}'"; fi
else
  ok "B13a  this machine cannot record a profile, and the script refuses"
  check_eq "B13b  and publishes nothing" "${before}" "${after}"
  printf '     profile.sh said:\n'; sed 's/^/       /' "${SCRATCH}/b13.err"
fi

# B16 -- a floor under the sample count, kept where the request floor is kept.
if (( BENCH_MIN_SAMPLES_SEEN >= 100 )); then
  ok "B16  the script keeps a floor under the sample count"
else
  bad "B16  sample floor" "BENCH_MIN_SAMPLES is ${BENCH_MIN_SAMPLES_SEEN}"
fi

# B17 -- the load has to still be running when the window closes AND to have
# done something in it. A background loop whose every iteration fails in
# milliseconds stays alive from start to finish and covers nothing. Driven as a
# pure function so all three ways of failing can be provoked exactly.
out="$(profile_load_ok 0 0 yes 2>&1)"; st=$?
check_nonzero_exit "B17a  a load that completed no rounds is refused" "${st}"
check_contains "B17b  and it says so" "${out}" "no iterations"

out="$(profile_load_ok 5 0 yes 2>&1)"; st=$?
check_nonzero_exit "B17c  a load that issued no requests is refused" "${st}"
check_contains "B17d  even though it was running" "${out}" "not loading"

out="$(profile_load_ok 5 5000 no 2>&1)"; st=$?
check_nonzero_exit "B17e  a load that finished early is refused" "${st}"
check_contains "B17f  and names the idle part of the window" "${out}" "idle"

profile_load_ok 5 5000 yes > /dev/null 2>&1
check_zero_exit "B17g  a load that covered the window is accepted" "$?"

# B17h -- counting rounds in a log that has none. `grep -c` prints its zero and
# THEN exits non-zero, so the obvious `|| echo 0` appends a second line and the
# arithmetic that follows dies with a syntax error -- on the one path this check
# exists to report. Asserted here because it is invisible everywhere else: the
# happy path never reaches it.
: > "${SCRATCH}/empty-load.log"
rounds="$(grep -c 'requests per second' "${SCRATCH}/empty-load.log" 2>/dev/null)" || rounds=0
if [[ "${rounds}" == "0" ]]; then ok "B17h  an empty load log counts as zero rounds, once"
else bad "B17h  empty load log" "counted '${rounds}', which will not survive arithmetic"; fi

echo "== profile.sh keeps its distance (case B24b) =="

# It is not one of run-all's measurements: it needs a runtime privilege the other
# four do not, and run-all failing by default would take bench_smoke down with it
# in all three builds.
if grep -qE 'for measurement in .*profile' "${HERE}/run-all.sh"; then
  bad "B24b run-all does not run the profiler" "profile is in run-all's list"
else
  ok "B24b run-all does not run the profiler"
fi
if [[ -x "${HERE}/profile.sh" ]]; then ok "B24c profile.sh is executable"
else bad "B24c profile.sh" "not executable; git update-index --chmod=+x benchmarks/profile.sh"; fi

echo
echo "passed ${passed}, failed ${failed}"
[[ "${failed}" -eq 0 ]]
