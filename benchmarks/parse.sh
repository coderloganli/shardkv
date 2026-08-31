#!/usr/bin/env bash
#
# Pure functions, sourced by the measurement scripts and by self_test.sh: reading
# redis-benchmark's output, and the rule that decides whether a run was local or
# remote.
#
# Nothing here starts a server or touches the network. That is deliberate -- the
# classification rule is the riskiest thing in this step, and a rule that can
# only be tested by getting the kernel to cooperate is a rule that is tested
# rarely and badly.
#
# Every one of these fails loudly and prints nothing on stdout when it cannot
# answer. A parser that returns an empty string for a run that died halfway puts
# a blank in a results file, and a blank in a results file looks like a
# measurement.

# The throughput line: "  throughput summary: 49504.95 requests per second"
parse_throughput() {
  local value
  value="$(sed -n 's/^ *throughput summary: *\([0-9.]*\) requests per second/\1/p' | head -1)"
  if [[ -z "${value}" ]]; then
    echo "parse_throughput: no throughput summary line in this output" >&2
    return 1
  fi
  printf '%s\n' "${value}"
}

# One column of the summary table:
#
#           avg       min       p50       p95       p99       max
#         0.140     0.016     0.135     0.231     0.391     1.575
#
# Note what is NOT here: p999. redis-benchmark 7.0.15's summary does not carry
# it and neither does --csv; parse_p999 below digs it out of the detailed block.
parse_percentile() {
  local want="$1"
  local column
  case "${want}" in
    avg) column=1 ;;
    min) column=2 ;;
    p50) column=3 ;;
    p95) column=4 ;;
    p99) column=5 ;;
    max) column=6 ;;
    *)
      echo "parse_percentile: unknown column '${want}'" >&2
      return 2
      ;;
  esac

  local value
  value="$(awk -v col="${column}" '
    /latency summary/ { seen = 1; next }
    seen && /^ *[0-9]/ { print $col; exit }
  ' | tr -d ' ')"

  if [[ -z "${value}" ]]; then
    echo "parse_percentile: no latency summary in this output" >&2
    return 1
  fi
  printf '%s\n' "${value}"
}

# p999, from the detailed percentile block:
#
#   99.902% <= 0.735 milliseconds (cumulative count 19982)
#
# The first entry at or above 99.9 percent. redis-benchmark prints rising
# percentiles, so the first one to reach 99.9 is the tightest bound it measured
# on that tail -- taking a later line would report a looser number as p999.
parse_p999() {
  local value
  # Field splitting rather than a capturing match(): a three-argument match() is
  # a gawk extension, and the image has mawk. A line reads
  #   99.902% <= 0.735 milliseconds (cumulative count 19982)
  # so the percentage is $1 with its sign attached and the figure is $3.
  value="$(awk '
    $2 == "<=" && $4 == "milliseconds" {
      pct = $1
      sub(/%$/, "", pct)
      if (pct + 0 >= 99.9) { print $3; exit }
    }
  ')"

  if [[ -z "${value}" ]]; then
    echo "parse_p999: no percentile at or above 99.9 in this output" >&2
    return 1
  fi
  printf '%s\n' "${value}"
}

# Was this run local or remote?
#
#   classify_run <counter delta> <requests> <shards>
#
# A run whose keys all belong to the connection's own loop sends no cross-shard
# messages; a run whose keys belong elsewhere sends one per request. So the delta
# separates them -- but not at zero, because the instrument is inside the
# experiment: INFO is itself a cross-shard command and each of the two bracket
# readings adds about (shards - 1) to the counter it is reading. See
# docs/adr/0015-the-cross-shard-penalty-is-observed-not-arranged.md
#
# The split is therefore at half the request count, which no amount of instrument
# cost can reach -- provided the request count is large enough for "half" to sit
# well above that cost. Below 100 * shards it is not, and the honest answer is
# "unknown" rather than a guess.
classify_run() {
  local delta="$1" requests="$2" shards="$3"

  if [[ -z "${delta}" || -z "${requests}" || -z "${shards}" ]]; then
    echo "classify_run: need <delta> <requests> <shards>" >&2
    return 2
  fi

  local floor=$((100 * shards))
  if (( requests < floor )); then
    printf 'unknown\n'
    return 0
  fi

  if (( delta * 2 < requests )); then
    printf 'local\n'
  else
    printf 'remote\n'
  fi
}

# median of a whitespace-separated list of numbers
_median() {
  tr ' ' '\n' <<< "$1" | grep -v '^$' | sort -g | awk '
    { v[NR] = $1 }
    END {
      if (NR == 0) exit 1
      if (NR % 2) print v[(NR + 1) / 2]
      else printf "%.4f\n", (v[NR / 2] + v[NR / 2 + 1]) / 2
    }'
}

_extreme() { tr ' ' '\n' <<< "$1" | grep -v '^$' | sort -g | { [[ "$2" == min ]] && head -1 || tail -1; }; }
_count()   { tr ' ' '\n' <<< "$1" | grep -v '^$' | wc -l | tr -d ' '; }

# The cross-shard penalty, from the two groups of per-run latencies.
#
#   penalty_report "<local latencies>" "<remote latencies>"
#
# Reports the count, median, minimum and maximum of both groups, and the
# difference of the medians. The spread is not decoration: each run is local with
# probability 1/shards, so the local group is the small one, and ten samples on a
# virtual machine make a median a reader deserves to be able to judge. When the
# two ranges overlap it says so, and the README does not then claim a penalty it
# cannot see.
#
# Fewer than three samples in either group is a refusal, naming the group. A
# difference computed from one sample is not a difference.
penalty_report() {
  local locals="$1" remotes="$2"
  local ln rn
  ln="$(_count "${locals}")"
  rn="$(_count "${remotes}")"

  local short=""
  (( ln < 3 )) && short="local"
  (( rn < 3 )) && short="${short:+${short} and }remote"
  if [[ -n "${short}" ]]; then
    echo "penalty_report: too few samples in the ${short} group (local ${ln}, remote ${rn}); not reporting a penalty" >&2
    return 1
  fi

  local lmed rmed lmin lmax rmin rmax
  lmed="$(_median "${locals}")"; lmin="$(_extreme "${locals}" min)"; lmax="$(_extreme "${locals}" max)"
  rmed="$(_median "${remotes}")"; rmin="$(_extreme "${remotes}" min)"; rmax="$(_extreme "${remotes}" max)"

  printf 'local_n: %s\n'      "${ln}"
  printf 'local_median: %s\n' "${lmed}"
  printf 'local_min: %s\n'    "${lmin}"
  printf 'local_max: %s\n'    "${lmax}"
  printf 'remote_n: %s\n'     "${rn}"
  printf 'remote_median: %s\n' "${rmed}"
  printf 'remote_min: %s\n'   "${rmin}"
  printf 'remote_max: %s\n'   "${rmax}"
  printf 'penalty_ms: %s\n'   "$(awk -v a="${rmed}" -v b="${lmed}" 'BEGIN { printf "%.4f", a - b }')"

  # Overlapping ranges mean the difference of medians is inside the noise. Said
  # here so that whoever writes the README cannot miss it.
  if awk -v lmax="${lmax}" -v rmin="${rmin}" 'BEGIN { exit !(lmax >= rmin) }'; then
    printf 'ranges_overlap: 1\n'
  else
    printf 'ranges_overlap: 0\n'
  fi
}
