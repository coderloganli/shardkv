#!/usr/bin/env bash
#
# Where this server's own time goes, at eight loops under load.
#
# Not one of run-all.sh's measurements, deliberately. The other four compare two
# programs; this one asks what one program spends its time on, so a control
# group would only take cores away from the thing being watched. It also needs a
# privilege the others do not -- see the refusals below -- and run-all failing by
# default on a machine without it would take the smoke test down with it in all
# three builds.
#
# WHAT THIS INSTRUMENT CAN AND CANNOT SEE is not a footnote here, it is the
# reason half this script exists:
#
#   docs/adr/0017-the-profile-sees-user-space-only.md
#
# In short: there is no PMU on this machine, so the sampling is driven by a
# software timer, and an unprivileged process may not sample the kernel. The
# recording therefore counts USER-MODE CPU TIME ONLY. A thread inside a system
# call accrues nothing while it is in there. The ordering of this program's own
# functions is what the profile supports; a share of total time is not, and the
# denominator is recorded beside the figures so that nobody has to take that on
# trust.
set -uo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# Which perf. Overridable so the refusal paths can be provoked by configuration
# rather than by editing a copy of this script.
#
# Candidates are TRIED, not merely found, and the one on PATH is tried last.
# `perf` on PATH is a wrapper that execs the build matching `uname -r`; inside a
# container that is the host's kernel, whose tools are not installed, so the
# wrapper is present, first in the search, and broken. Picking it because it
# exists produces a confident refusal on a machine that can profile perfectly
# well -- which is exactly what this script did on its first run.
find_perf() {
  local candidate
  if [[ -n "${BENCH_PERF:-}" ]]; then printf '%s\n' "${BENCH_PERF}"; return 0; fi
  for candidate in /usr/lib/linux-tools/*/perf "$(command -v perf 2>/dev/null)"; do
    [[ -x "${candidate}" ]] || continue
    if "${candidate}" --version > /dev/null 2>&1; then
      printf '%s\n' "${candidate}"
      return 0
    fi
  done
  # Nothing worked. Name the one that at least exists, so the refusal can say
  # something more useful than "no perf" about a machine that has one.
  for candidate in /usr/lib/linux-tools/*/perf "$(command -v perf 2>/dev/null)"; do
    [[ -x "${candidate}" ]] && { printf '%s\n' "${candidate}"; return 0; }
  done
}

PERF="$(find_perf)"
if [[ -z "${PERF}" || ! -x "${PERF}" ]]; then
  bench_die "profile.sh: no perf executable (looked on PATH and in /usr/lib/linux-tools/*/perf).
  Install it -- linux-tools-generic on Ubuntu -- or set BENCH_PERF to name it."
fi

# Can it actually open an event? Asked before anything is started, because the
# answer is no far more often than people expect and the failure arrives with a
# message that points at the wrong thing.
#
# What perf says when the container's syscall filter blocks perf_event_open is
# "No permission to enable task-clock event." -- which reads as a kernel
# permission setting, and sends people to change one that is already correct.
# So the candidates are listed rather than a single cause being asserted: this
# same failure comes from the filter, from perf_event_paranoid, and from a
# kernel whose tooling is not installed, and this script cannot tell which
# without guessing.
probe="$("${PERF}" stat -e task-clock -- true 2>&1)"
if [[ $? -ne 0 ]] || printf '%s' "${probe}" | grep -qi 'permission\|not permitted\|not supported'; then
  paranoid="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
  bench_die "profile.sh: perf could not open an event. It said:
  ${probe}
  This machine reports perf_event_paranoid=${paranoid}. Any of these will produce that message:
    - the container's seccomp profile blocking perf_event_open
      (docker run --security-opt seccomp=unconfined ...)
    - perf_event_paranoid set above 2
    - a kernel without the matching perf tooling installed
  Which one it is, this script cannot tell without guessing, so it does not."
fi

bench_begin
raw="${BENCH_WORK}/profile.raw"
out="${BENCH_WORK}/profile.txt"
data="${BENCH_WORK}/perf.data"
record_err="${BENCH_WORK}/perf-record.err"
load_log="${BENCH_WORK}/load.log"

bench_start_shardkv

# CPU time as the process itself accounts for it, which is the only way to size
# the part of the picture the profile cannot see. Fields 14 and 15 of
# /proc/<pid>/stat are utime and stime, in clock ticks.
# Fields 14 and 15 of /proc/<pid>/stat are utime and stime -- counted from the
# START of the line, which is where this gets interesting: field 2 is the
# executable's name in parentheses, and a name containing a space or a bracket
# shifts every field after it. "shardkv" contains neither, so splitting on
# whitespace would work today and break the day somebody renames the binary.
# Everything up to the last ')' is dropped instead, which is what /proc(5)
# itself recommends, and the fields are then counted from the state character.
cpu_ticks() { # pid field
  local line rest
  line="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
  rest="${line##*) }"
  awk -v f="$(( $2 - 2 ))" '{ print $(f) }' <<< "${rest}"
}
ticks_per_second="$(getconf CLK_TCK 2>/dev/null || echo 100)"

user_before="$(cpu_ticks "${BENCH_SHARDKV_PID}" 14)"
sys_before="$(cpu_ticks "${BENCH_SHARDKV_PID}" 15)"

# The load runs in a loop for longer than the window, so the window is covered
# end to end. Its output is kept: how many rounds finished and how many requests
# they carried is what says the load was really there, and a live process does
# not say that.
# The generator's own pid is written down each round, because killing the shell
# that started it does not reliably kill it: `pkill -P` needs the child still to
# be a child, and a round that is mid-exec or has been reparented is neither. A
# surviving generator would keep hammering the port, and the next run refuses to
# start on a port something is already answering on -- so one stray process
# breaks every run after it, for a reason that looks nothing like its cause.
load_child="${BENCH_WORK}/load.pid"
(
  while :; do
    redis-benchmark -p "${BENCH_SHARDKV_PORT}" -t set \
      -n "${BENCH_REQUESTS}" -c "${BENCH_CLIENTS}" \
      --threads "${BENCH_GENERATOR_THREADS}" -q >> "${load_log}" 2>&1 &
    printf '%s' "$!" > "${load_child}"
    wait $! || break
  done
) &
load_pid=$!

sleep 1  # let the first round get going, so the window opens onto real work

"${PERF}" record -e cpu-clock -F 999 -g -p "${BENCH_SHARDKV_PID}" \
  -o "${data}" -- sleep "${BENCH_PROFILE_SECONDS}" 2> "${record_err}"

# Whether the load was still going when the window closed, asked before it is
# stopped.
load_still_running=no
kill -0 "${load_pid}" 2>/dev/null && load_still_running=yes
kill "${load_pid}" 2>/dev/null
pkill -P "${load_pid}" 2>/dev/null
[[ -s "${load_child}" ]] && kill "$(cat "${load_child}")" 2>/dev/null
wait "${load_pid}" 2>/dev/null
# And wait for the port to go quiet, for the reason bench_cleanup does the same:
# the next run refuses a port that still answers, so leaving before the sockets
# close makes two runs in a row fail for no visible reason.
for _ in $(seq 1 50); do
  pgrep -f "redis-benchmark -p ${BENCH_SHARDKV_PORT}" > /dev/null 2>&1 || break
  sleep 0.1
done

user_after="$(cpu_ticks "${BENCH_SHARDKV_PID}" 14)"
sys_after="$(cpu_ticks "${BENCH_SHARDKV_PID}" 15)"

# `grep -c` exits non-zero when it counts nothing, and it has ALREADY printed
# the zero by then -- so `|| echo 0` appends a second line and the arithmetic
# below dies with a syntax error. On the one path that matters most: the one
# where the load did nothing, which is exactly the case this is here to report.
load_rounds="$(grep -c 'requests per second' "${load_log}" 2>/dev/null)" || load_rounds=0
load_requests=$(( load_rounds * BENCH_REQUESTS ))
profile_load_ok "${load_rounds}" "${load_requests}" "${load_still_running}" || {
  bench_explain "the load" "$(tail -5 "${load_log}" 2>/dev/null)"
  bench_die "profile.sh: the recording window was not covered by load; nothing is recorded"
}

samples="$(parse_profile_samples < "${record_err}")" \
  || bench_die "profile.sh: the recording captured nothing"
if (( samples < BENCH_MIN_SAMPLES )); then
  bench_die "profile.sh: ${samples} samples is below the floor of ${BENCH_MIN_SAMPLES}; an ordering read off this many is noise. Raise BENCH_PROFILE_SECONDS."
fi

"${PERF}" report -i "${data}" --stdio --no-children -g none --percent-limit 0.3 \
  > "${raw}" 2>/dev/null \
  || bench_die "profile.sh: perf report could not read the recording"
rm -f "${data}"

event="$(parse_profile_event < "${raw}")" || bench_die "profile.sh: the profile's scope is unknown"
event_count="$(parse_profile_event_count < "${raw}")" \
  || bench_die "profile.sh: the profile has no denominator"
unresolved="$(parse_profile_unresolved < "${raw}")" \
  || bench_die "profile.sh: the profile has no ranked symbols"

# The event's name is checked rather than recorded as a figure, because every
# claim the README makes from this table rests on the recording having been
# user-space only, and a table whose scope silently changed would look exactly
# the same.
case "${event}" in
  *:u) ;;
  *) bench_die "profile.sh: this recording is '${event}', not a user-space-only event; docs/adr/0017 describes what this project claims from a profile and it assumes ':u'" ;;
esac

if awk -v u="${unresolved}" -v m="${BENCH_MAX_UNRESOLVED}" 'BEGIN{exit !(u > m)}'; then
  bench_die "profile.sh: ${unresolved}% of the profile landed on addresses with no name (limit ${BENCH_MAX_UNRESOLVED}%); the numbers are there and mean nothing. Build with debug information."
fi

bench_number profile_samples "${samples}" >> "${out}"
bench_number profile_seconds "${BENCH_PROFILE_SECONDS}" >> "${out}"
bench_number shards "${BENCH_SHARDS}" >> "${out}"
bench_number load_rounds "${load_rounds}" >> "${out}"
bench_number load_requests "${load_requests}" >> "${out}"
bench_number unresolved_percent "${unresolved}" >> "${out}"

# The denominator, three ways, because the table above is a share of the first
# of them and a reader will assume it is a share of the third.
bench_number sampled_user_ns "${event_count}" >> "${out}"
awk -v a="${user_after}" -v b="${user_before}" -v t="${ticks_per_second}" \
    'BEGIN{ printf "user_cpu_seconds: %.3f\n", (a - b) / t }' >> "${out}"
awk -v a="${sys_after}" -v b="${sys_before}" -v t="${ticks_per_second}" \
    'BEGIN{ printf "kernel_cpu_seconds: %.3f\n", (a - b) / t }' >> "${out}"

bench_finish
