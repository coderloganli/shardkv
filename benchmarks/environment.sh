#!/usr/bin/env bash
#
# The environment block that goes beside every set of figures.
#
# A number without its environment is not a number: the same benchmark on a
# different kernel, a different compiler, or with the load generator on another
# machine is a different experiment. docs/product.md states the principle; this
# is the part that makes it happen rather than being remembered.
#
# It FAILS, loudly and with a non-zero status, when it cannot determine any
# field. A partial block is worse than none -- it looks like a record and is not
# one -- and the measurement scripts are built so that a failure here leaves no
# results directory behind at all.
#
# Overridable for testing: BENCH_REDIS_SERVER_VERSION and
# BENCH_REDIS_BENCHMARK_VERSION, and BENCH_CPU_PROC / BENCH_CPU_LSCPU, which
# point the two CPU detectors at files so that a machine which CAN name its CPU
# can still exercise the path of one that cannot. BENCH_CPU_MODEL is not a test
# hook: it is the operator's assertion, and is documented in
# docs/adr/0018-an-environment-field-may-be-asserted-never-guessed.md.
# BUILD_DIR names the build tree the compiler and optimisation level are read
# from.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO}/build}"

missing=()

emit() { # name value
  if [[ -z "${2:-}" ]]; then
    missing+=("$1")
  else
    printf '%s: %s\n' "$1" "$2"
  fi
}

# The CPU, by assertion first and detection second.
#
# Assertion first is the same rule the commit hash already follows, and it is
# one rule rather than two: a precedence that changed depending on whether
# detection happened to succeed would be a precedence nobody could predict. The
# operator knows what machine they are sitting at better than a guest kernel
# does, and setting the variable is a deliberate act.
#
# Detection is two sources because one is not enough. `/proc/cpuinfo` carries no
# `model name` line at all on aarch64, and `lscpu` answers "-" under a desktop
# hypervisor that tells its guest nothing -- which is the machine this was
# written on. Neither is a failure of the recorder; the machine genuinely cannot
# say what it is, and the assertion exists for exactly that.
cpu_proc="${BENCH_CPU_PROC:-/proc/cpuinfo}"
cpu_model_asserted=no

if [[ -n "${BENCH_CPU_MODEL:-}" ]]; then
  # Checked before a single line of the block is printed. The block is
  # `name: value` lines, so a value carrying a newline forges a field the
  # recorder never wrote -- and a recorder that prints half a block before
  # noticing has still put half a block somewhere a careless caller could keep.
  if [[ "${BENCH_CPU_MODEL}" == *$'\n'* || "${BENCH_CPU_MODEL}" == *:* ]]; then
    printf 'environment.sh: refusing BENCH_CPU_MODEL: a newline or a colon in it would forge a field\n' >&2
    exit 1
  fi
  cpu_model="${BENCH_CPU_MODEL} (asserted)"
  cpu_model_asserted=yes
  printf 'environment.sh: cpu_model was asserted, not detected: %s\n' "${BENCH_CPU_MODEL}" >&2
else
  cpu_model="$(sed -n 's/^model name[[:space:]]*: *//p' "${cpu_proc}" 2>/dev/null | head -1)"
  if [[ -z "${cpu_model}" ]]; then
    if [[ -n "${BENCH_CPU_LSCPU:-}" ]]; then
      lscpu_out="$(cat "${BENCH_CPU_LSCPU}" 2>/dev/null)"
    else
      lscpu_out="$(lscpu 2>/dev/null)"
    fi
    cpu_model="$(printf '%s\n' "${lscpu_out}" | sed -n 's/^Model name:[[:space:]]*//p' | head -1)"
    # "-" and "unknown" are what a hypervisor says when it is not telling. They
    # are placeholders, not answers, and recording one would be exactly the
    # vague value this whole mechanism exists to keep out.
    case "${cpu_model}" in
      -|unknown|Unknown|UNKNOWN) cpu_model="" ;;
    esac
  fi
fi
cpu_cores="$(nproc 2>/dev/null)"
kernel="$(uname -sr 2>/dev/null)"

# From the build tree rather than from whatever compiler happens to be on PATH:
# the figures came out of a particular build, and that build recorded what made
# it. `which g++` would answer a different question.
cache="${BUILD_DIR}/CMakeCache.txt"
if [[ -r "${cache}" ]]; then
  compiler="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "${cache}" | head -1)"
  build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[^=]*=//p' "${cache}" | head -1)"
  [[ -z "${build_type}" ]] && build_type="Release"  # the CMakeLists default
  sanitizer="$(sed -n 's/^SHARDKV_SANITIZER:[^=]*=//p' "${cache}" | head -1)"
  [[ -z "${sanitizer}" ]] && sanitizer="none"
else
  compiler=""
  build_type=""
  sanitizer=""
fi

compiler_version=""
if [[ -n "${compiler}" && -x "${compiler}" ]]; then
  compiler_version="$("${compiler}" --version 2>/dev/null | head -1)"
fi

redis_server_version="${BENCH_REDIS_SERVER_VERSION:-$(redis-server --version 2>/dev/null | sed -n 's/.*v=\([0-9.]*\).*/\1/p')}"
redis_benchmark_version="${BENCH_REDIS_BENCHMARK_VERSION:-$(redis-benchmark --version 2>/dev/null | sed -n 's/^redis-benchmark *\([0-9.]*\).*/\1/p')}"

# Provenance, and the one field allowed to say it does not know.
#
# git normally answers this. It cannot when the repository is a linked worktree
# mounted into a container without its parent -- the .git file points at a path
# outside the mount -- which is exactly how this is developed. Recording
# "unavailable" is honest and is still a record; a blank compiler would not be,
# because the compiler is part of what makes the numbers mean anything.
# BENCH_SHARDKV_COMMIT overrides it, and a warning goes to stderr so that a real
# recorded run cannot lose its provenance quietly.
shardkv_commit="${BENCH_SHARDKV_COMMIT:-$(git -C "${REPO}" rev-parse --short HEAD 2>/dev/null)}"
if [[ -n "${shardkv_commit}" ]]; then
  [[ -n "$(git -C "${REPO}" status --porcelain 2>/dev/null)" ]] && shardkv_commit="${shardkv_commit}-dirty"
else
  shardkv_commit="unavailable"
  echo "environment.sh: git could not name the commit; recording it as unavailable" >&2
fi

emit cpu_model "${cpu_model}"
emit cpu_cores "${cpu_cores}"
emit kernel "${kernel}"
emit compiler "${compiler}"
emit compiler_version "${compiler_version}"
emit build_type "${build_type}"
emit sanitizer "${sanitizer}"
emit redis_server_version "${redis_server_version}"
emit redis_benchmark_version "${redis_benchmark_version}"

# Always true here and recorded anyway, because it is the single fact that most
# changes what the absolute figures mean: the load generator competes with the
# server for the same cores. The control group runs under the same competition,
# which is why the DIFFERENCE survives and the absolutes are written down as
# depressed.
emit same_machine "yes"
emit pinned "${BENCH_PINNED:-no}"
emit shardkv_commit "${shardkv_commit}"
emit date "$(date -u +%Y-%m-%dT%H:%M:%SZ)"

if (( ${#missing[@]} > 0 )); then
  printf 'environment.sh: could not determine %s\n' "${missing[*]}" >&2
  printf 'environment.sh: BUILD_DIR=%s (needs a configured build tree)\n' "${BUILD_DIR}" >&2
  # Naming the field and stopping there leaves the person who hit this with no
  # next step, which is how a correct refusal turns into an unusable machine.
  for name in "${missing[@]}"; do
    if [[ "${name}" == "cpu_model" ]]; then
      printf 'environment.sh: this machine does not say what CPU it is. Set BENCH_CPU_MODEL to assert it;\n' >&2
      printf 'environment.sh: the record will show the value as asserted rather than detected.\n' >&2
    fi
  done
  exit 1
fi

# A control group on a different version than the load generator is a different
# experiment from the one being claimed, so this is a refusal and not a warning.
if [[ "${redis_server_version}" != "${redis_benchmark_version}" ]]; then
  printf 'environment.sh: redis-server is %s but redis-benchmark is %s; the control group and the load generator must be the same version\n' \
    "${redis_server_version}" "${redis_benchmark_version}" >&2
  exit 1
fi
