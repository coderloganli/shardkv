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
# BENCH_REDIS_BENCHMARK_VERSION. BUILD_DIR names the build tree the compiler and
# optimisation level are read from.
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

cpu_model="$(sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo | head -1)"
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
  exit 1
fi

# A control group on a different version than the load generator is a different
# experiment from the one being claimed, so this is a refusal and not a warning.
if [[ "${redis_server_version}" != "${redis_benchmark_version}" ]]; then
  printf 'environment.sh: redis-server is %s but redis-benchmark is %s; the control group and the load generator must be the same version\n' \
    "${redis_server_version}" "${redis_benchmark_version}" >&2
  exit 1
fi
