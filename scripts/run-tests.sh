#!/usr/bin/env bash
# ============================================================
# Cabe local test runner —— Fedora 43 only.
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

# ---------- distro check ----------
if [[ -f /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    if [[ "${ID:-}" != "fedora" || "${VERSION_ID:-0}" -lt 43 ]]; then
        echo "Error: Cabe requires Fedora 43+. Detected: ${ID:-unknown} ${VERSION_ID:-?}" >&2
        exit 1
    fi
fi

# ---------- defaults ----------
BUILD_TYPE="Debug"
SAN_FLAG=""
SAN_SUFFIX=""
CLEAN=false
FILTER=""
JOBS="$(nproc)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release) BUILD_TYPE="Release" ;;
        --debug)   BUILD_TYPE="Debug" ;;
        --asan)    SAN_FLAG="-DCABE_ENABLE_ASAN=ON";  SAN_SUFFIX="-asan" ;;
        --tsan)    SAN_FLAG="-DCABE_ENABLE_TSAN=ON";  SAN_SUFFIX="-tsan" ;;
        --ubsan)   SAN_FLAG="-DCABE_ENABLE_UBSAN=ON"; SAN_SUFFIX="-ubsan" ;;
        --clean)   CLEAN=true ;;
        --filter)  FILTER="$2"; shift ;;
        --jobs)    JOBS="$2"; shift ;;
        -h|--help)
            cat <<EOF
Usage: $0 [--release|--debug] [--asan|--tsan|--ubsan] [--clean] [--filter REGEX] [--jobs N]
EOF
            exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
    shift
done

BUILD_DIR="$ROOT/build${SAN_SUFFIX}"
echo "=== build_type=$BUILD_TYPE san=${SAN_SUFFIX:-none} dir=$BUILD_DIR jobs=$JOBS ==="

[[ "$CLEAN" == "true" && -d "$BUILD_DIR" ]] && { echo ">>> rm -rf $BUILD_DIR"; rm -rf "$BUILD_DIR"; }

# shellcheck disable=SC2086
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    $SAN_FLAG

cmake --build "$BUILD_DIR" --parallel "$JOBS"

cd "$BUILD_DIR"
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:second_deadlock_stack=1}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1:detect_leaks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:print_stacktrace=1}"

CTEST_ARGS=(--output-on-failure --parallel "$JOBS")
[[ -n "$FILTER" ]] && CTEST_ARGS+=(-R "$FILTER")
ctest "${CTEST_ARGS[@]}"