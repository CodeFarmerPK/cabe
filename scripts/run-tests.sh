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
BACKEND="sync"           # P3 M4: IoBackend 选择,默认 sync
BACKEND_SUFFIX=""        # 默认 backend 不进 build 目录名(向后兼容旧 build/ 目录)
CLEAN=false
FILTER=""
JOBS="$(nproc)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)        BUILD_TYPE="Release" ;;
        --debug)          BUILD_TYPE="Debug" ;;
        --asan)           SAN_FLAG="-DCABE_ENABLE_ASAN=ON";  SAN_SUFFIX="-asan" ;;
        --tsan)           SAN_FLAG="-DCABE_ENABLE_TSAN=ON";  SAN_SUFFIX="-tsan" ;;
        --ubsan)          SAN_FLAG="-DCABE_ENABLE_UBSAN=ON"; SAN_SUFFIX="-ubsan" ;;
        --backend)        BACKEND="$2"; shift ;;
        --backend=*)      BACKEND="${1#*=}" ;;
        --clean)          CLEAN=true ;;
        --filter)         FILTER="$2"; shift ;;
        --jobs)           JOBS="$2"; shift ;;
        -h|--help)
            cat <<EOF
Usage: $0 [--release|--debug] [--asan|--tsan|--ubsan] [--backend=BACKEND]
          [--clean] [--filter REGEX] [--jobs N]

Options:
  --release|--debug    Build type (default: Debug)
  --asan|--tsan|--ubsan
                       Enable specific sanitizer (mutually exclusive)
  --backend=BACKEND    IoBackend selection: sync (default) | io_uring | spdk
                       P3 stage only sync is implemented;
                       io_uring/spdk will trigger CMake FATAL_ERROR until P4/P9.
  --clean              Remove build directory before configuring
  --filter REGEX       Run only tests matching POSIX regex
  --jobs N             Parallel build jobs (default: $(nproc))

Build directory naming:
  build, build-asan                  (sync, default backend - suffix omitted)
  build-io_uring, build-io_uring-asan (non-default backend in name)
EOF
            exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
    shift
done

# 非默认 backend 加进 build 目录名,允许多 backend 并存:
#   build, build-asan       (sync 默认,后缀省略)
#   build-io_uring          (io_uring 后端)
#   build-io_uring-asan     (io_uring + ASAN)
[[ "$BACKEND" != "sync" ]] && BACKEND_SUFFIX="-${BACKEND}"

BUILD_DIR="$ROOT/build${BACKEND_SUFFIX}${SAN_SUFFIX}"
echo "=== build_type=$BUILD_TYPE backend=$BACKEND san=${SAN_SUFFIX:-none} dir=$BUILD_DIR jobs=$JOBS ==="

[[ "$CLEAN" == "true" && -d "$BUILD_DIR" ]] && { echo ">>> rm -rf $BUILD_DIR"; rm -rf "$BUILD_DIR"; }

# shellcheck disable=SC2086
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCABE_IO_BACKEND="$BACKEND" \
    $SAN_FLAG

cmake --build "$BUILD_DIR" --parallel "$JOBS"

cd "$BUILD_DIR"
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:second_deadlock_stack=1}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1:detect_leaks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:print_stacktrace=1}"

# ---------- 裸设备依赖提示 ----------
# Cabe 是裸块设备引擎,Engine 集成测试需要 CABE_TEST_DEVICE 指向真实块设备。
# 未设置时 Engine* 测试会自动 SKIP,但纯 unit 测试(util / memory / buffer / freelist)
# 仍会跑。开发期推荐用 scripts/mkloop.sh 创建 loop device。
if [[ -z "${CABE_TEST_DEVICE:-}" ]]; then
    echo ""
    echo "[hint] CABE_TEST_DEVICE not set. Engine integration tests will be SKIPPED."
    echo "       To enable them:"
    echo "         sudo ./scripts/mkloop.sh create"
    echo "         export CABE_TEST_DEVICE=/dev/loopX  # see mkloop output"
    echo "         ./scripts/run-tests.sh"
    echo ""
fi

# 串行执行:同一裸设备不能被多个 Engine test 并发占用(它们都会 Open 同一
# 设备,nextBlockId_ / FreeList 互相覆盖)。fixture 内的多线程并发不受影响。
CTEST_ARGS=(--output-on-failure --parallel 1)
[[ -n "$FILTER" ]] && CTEST_ARGS+=(-R "$FILTER")
ctest "${CTEST_ARGS[@]}"