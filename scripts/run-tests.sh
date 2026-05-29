#!/usr/bin/env bash
# ============================================================
# Cabe —— 本地测试运行脚本
# 单次调用跑一个配置；多配置通过多次调用组合。
# 设计依据：doc/P0/P0M6_test_scripts_design.md §4（P1 改造）
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

usage() {
    cat <<EOF
用法: scripts/run-tests.sh [选项]

构建类型:
  --release           Release 构建（默认 Debug）
  --debug             Debug 构建（默认）

检测器（互斥）:
  --asan              启用 AddressSanitizer
  --tsan              启用 ThreadSanitizer
  --ubsan             启用 UndefinedBehaviorSanitizer
                      不传则不开检测器

后端（P3+ 生效）:
  --backend=NAME      IoBackend 选择: sync（默认）| io_uring | spdk
                      P1-P2 阶段仅 sync 可用；其他值 CMake 会报错

工具链（可选）:
  --compiler=NAME     指定编译器: g++（默认）/ clang++
                      不传则用系统默认 g++

构建控制:
  --clean             跑前删除 build 目录重建
  --jobs N            并行构建线程数（默认 $(nproc)）

设备（P5 起一个设备组 = 数据 + WAL + 快照三块）:
  --device=PATH           数据设备路径（如 /dev/loop0）
  --wal-device=PATH       WAL 设备路径（如 /dev/loop1）
  --snapshot-device=PATH  快照设备路径（如 /dev/loop2）
                          需设备的测试要三者齐备，否则跳过
                          可用 ./scripts/mkloop.sh create 一键创建三块

测试过滤:
  --filter REGEX      只跑匹配 POSIX 正则的测试用例
                      例: --filter 'Engine*'  --filter 'Status.*'

其他:
  -h, --help          输出本用法

构建目录命名（工具链 + 后端 + 检测器组合）:
  build                         (g++ + sync + 无检测器)
  build-asan                    (g++ + sync + ASAN)
  build-clang                   (clang++ + sync + 无检测器)
  build-clang-asan              (clang++ + sync + ASAN)
  build-io_uring                (g++ + io_uring + 无检测器)
  build-clang-io_uring-asan     (clang++ + io_uring + ASAN)

示例（设备路径取自 ./scripts/mkloop.sh create 的输出）:
  ./scripts/run-tests.sh --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2
  ./scripts/run-tests.sh --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --asan
  ./scripts/run-tests.sh --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --backend=io_uring
  ./scripts/run-tests.sh --compiler=clang++ --asan                    # clang++ + ASAN（无设备测试跳过）
  ./scripts/run-tests.sh --release                                    # Release（跳过设备测试）
  ./scripts/run-tests.sh --filter 'Engine*' --device=...              # 只跑 Engine 用例
  ./scripts/run-tests.sh --clean --asan                               # 清理后重建 + ASAN

退出码:
  0  全部 PASS
  1  任一 FAIL
  2  参数错误 / 不兼容组合
EOF
}

# ---------- 默认值 ----------
BUILD_TYPE="Debug"
SANITIZER="none"
SAN_SUFFIX=""
COMPILER="g++"
COMPILER_SUFFIX=""
BACKEND="sync"
BACKEND_SUFFIX=""
CLEAN=false
DEVICE=""
WAL_DEVICE=""
SNAPSHOT_DEVICE=""
FILTER=""
JOBS="$(nproc)"

# ---------- 参数解析 ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)    BUILD_TYPE="Release" ;;
        --debug)      BUILD_TYPE="Debug" ;;
        --asan)       SANITIZER="address";   SAN_SUFFIX="-asan" ;;
        --tsan)       SANITIZER="thread";    SAN_SUFFIX="-tsan" ;;
        --ubsan)      SANITIZER="undefined"; SAN_SUFFIX="-ubsan" ;;
        --compiler=*) COMPILER="${1#*=}" ;;
        --compiler)   COMPILER="$2"; shift ;;
        --backend=*)  BACKEND="${1#*=}" ;;
        --backend)    BACKEND="$2"; shift ;;
        --device=*)            DEVICE="${1#*=}" ;;
        --device)              DEVICE="$2"; shift ;;
        --wal-device=*)        WAL_DEVICE="${1#*=}" ;;
        --wal-device)          WAL_DEVICE="$2"; shift ;;
        --snapshot-device=*)   SNAPSHOT_DEVICE="${1#*=}" ;;
        --snapshot-device)     SNAPSHOT_DEVICE="$2"; shift ;;
        --clean)      CLEAN=true ;;
        --filter=*)   FILTER="${1#*=}" ;;
        --filter)     FILTER="$2"; shift ;;
        --jobs=*)     JOBS="${1#*=}" ;;
        --jobs)       JOBS="$2"; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "Error: 未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---------- 工具链校验 ----------
case "$COMPILER" in
    g++|clang++) ;;
    *) echo "Error: --compiler= 仅接受 g++ / clang++（got: $COMPILER）" >&2; exit 2 ;;
esac
[[ "$COMPILER" != "g++" ]] && COMPILER_SUFFIX="-clang"

# ---------- TSAN + io_uring 不兼容前置拒绝（P4 D19 预留） ----------
if [[ "$BACKEND" == "io_uring" && "$SAN_SUFFIX" == "-tsan" ]]; then
    cat >&2 <<'EOF'
Error: --backend=io_uring 与 --tsan 不兼容。

io_uring 的 SQ/CQ 是用户态与内核共享内存，TSAN 看不到内核侧 store，
会产生大量误报。

可用组合:
  ./scripts/run-tests.sh --tsan                      (sync + TSAN)
  ./scripts/run-tests.sh --backend=io_uring --asan   (io_uring + ASAN)
  ./scripts/run-tests.sh --backend=io_uring --ubsan  (io_uring + UBSAN)
EOF
    exit 2
fi

# ---------- 构建目录 ----------
[[ "$BACKEND" != "sync" ]] && BACKEND_SUFFIX="-${BACKEND}"
BUILD_DIR="$ROOT/build${COMPILER_SUFFIX}${BACKEND_SUFFIX}${SAN_SUFFIX}"

echo "=== compiler=$COMPILER build_type=$BUILD_TYPE sanitizer=${SANITIZER} backend=$BACKEND device=${DEVICE:-（未指定）} wal=${WAL_DEVICE:-（未指定）} snapshot=${SNAPSHOT_DEVICE:-（未指定）} dir=$BUILD_DIR jobs=$JOBS ==="

# ---------- 清理（可选） ----------
if [[ "$CLEAN" == "true" && -d "$BUILD_DIR" ]]; then
    echo ">>> rm -rf $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# ---------- 配置 + 构建 ----------
cmake_args=(
    -S "$ROOT" -B "$BUILD_DIR" -G Ninja
    -DCMAKE_CXX_COMPILER="$COMPILER"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCABE_BUILD_TESTS=ON
    -DCABE_IO_BACKEND="$BACKEND"
)
[[ "$SANITIZER" != "none" ]] && cmake_args+=(-DCABE_SANITIZER="$SANITIZER")

cmake "${cmake_args[@]}"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

# ---------- 检测器运行时选项 ----------
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:second_deadlock_stack=1}"
export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1:detect_leaks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:print_stacktrace=1}"

# ---------- 设备环境变量 ----------
[[ -n "$DEVICE" ]]          && export CABE_TEST_DEVICE="$DEVICE"
[[ -n "$WAL_DEVICE" ]]      && export CABE_TEST_WAL_DEVICE="$WAL_DEVICE"
[[ -n "$SNAPSHOT_DEVICE" ]] && export CABE_TEST_SNAPSHOT_DEVICE="$SNAPSHOT_DEVICE"

# ---------- 跑测试 ----------
ctest_args=(--test-dir "$BUILD_DIR" --output-on-failure)
[[ -n "$FILTER" ]] && ctest_args+=(-R "$FILTER")

ctest "${ctest_args[@]}"
