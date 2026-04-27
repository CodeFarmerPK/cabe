#!/usr/bin/env bash
# ============================================================
# Cabe benchmark runner —— Fedora 43 only.
#
# Forces Release build, disables sanitizers, pins to a single CPU
# via taskset. Optional: --archive LABEL writes JSON + env report
# to bench/baselines/.
#
# Usage:
#   ./scripts/run-bench.sh                         # all, console only
#   ./scripts/run-bench.sh --filter 'CRC32'        # subset
#   ./scripts/run-bench.sh --archive p1.0-post     # archive to JSON
#   ./scripts/run-bench.sh --clean                 # wipe build-bench/
#
# Env overrides:
#   CPU_PIN   CPU index for taskset (default: 0)
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
FILTER=""
ARCHIVE_LABEL=""
CLEAN=false
CPU_PIN="${CPU_PIN:-0}"
JOBS="$(nproc)"
BACKEND="sync"            # P3 M4: IoBackend 选择,默认 sync
BACKEND_SUFFIX=""         # 非默认 backend 进 build 目录名

# ---------- arg parsing ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --filter)         FILTER="$2"; shift ;;
        --archive)        ARCHIVE_LABEL="$2"; shift ;;
        --clean)          CLEAN=true ;;
        --cpu)            CPU_PIN="$2"; shift ;;
        --backend)        BACKEND="$2"; shift ;;
        --backend=*)      BACKEND="${1#*=}" ;;
        -h|--help)
            cat <<EOF
Usage: $0 [--filter REGEX] [--archive LABEL] [--clean] [--cpu N] [--backend=BACKEND]

  --filter REGEX     Run only benchmarks matching regex (gbench style)
  --archive LABEL    Archive results to bench/baselines/LABEL-TIMESTAMP.json
  --clean            Wipe build-bench[-BACKEND]/ before configuring
  --cpu N            Pin to CPU N (default: 0)
  --backend=BACKEND  IoBackend selection: sync (default) | io_uring | spdk
                     P3 stage only sync is implemented;
                     io_uring/spdk trigger CMake FATAL_ERROR until P4/P9.

Build directory naming:
  build-bench               (sync, default backend - suffix omitted)
  build-bench-io_uring      (non-default backend in name)
EOF
            exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 1 ;;
    esac
    shift
done

# 非默认 backend 加 build 目录后缀,与 run-tests.sh 命名约定一致
[[ "$BACKEND" != "sync" ]] && BACKEND_SUFFIX="-${BACKEND}"
BUILD_DIR="$ROOT/build-bench${BACKEND_SUFFIX}"

# ---------- clean ----------
if [[ "$CLEAN" == "true" && -d "$BUILD_DIR" ]]; then
    echo ">>> rm -rf $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# ---------- configure (Release, no sanitizers) ----------
echo "=== Configuring $BUILD_DIR (Release, backend=$BACKEND) ==="
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCABE_IO_BACKEND="$BACKEND" \
    -DCABE_ENABLE_ASAN=OFF \
    -DCABE_ENABLE_TSAN=OFF \
    -DCABE_ENABLE_UBSAN=OFF \
    -DCABE_BUILD_BENCH=ON \
    -DCABE_BUILD_TESTS=OFF

echo "=== Building cabe_bench ==="
cmake --build "$BUILD_DIR" --target cabe_bench --parallel "$JOBS"

BENCH_BIN="$BUILD_DIR/cabe_bench"
if [[ ! -x "$BENCH_BIN" ]]; then
    echo "Error: $BENCH_BIN not found after build" >&2
    exit 1
fi

# ---------- build arg list ----------
BENCH_ARGS=(--benchmark_counters_tabular=true)
if [[ -n "$FILTER" ]]; then
    BENCH_ARGS+=(--benchmark_filter="$FILTER")
fi

# ---------- archive mode: prepare JSON + env report ----------
if [[ -n "$ARCHIVE_LABEL" ]]; then
    TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
    OUT_DIR="$ROOT/bench/baselines"
    mkdir -p "$OUT_DIR"
    OUT_JSON="$OUT_DIR/${ARCHIVE_LABEL}-${TIMESTAMP}.json"
    OUT_ENV="$OUT_DIR/${ARCHIVE_LABEL}-${TIMESTAMP}.env.txt"

    BENCH_ARGS+=(--benchmark_format=json "--benchmark_out=$OUT_JSON")

    {
        echo "=== Cabe benchmark environment ==="
        echo "Timestamp:     $(date --iso-8601=seconds)"
        echo "Archive label: $ARCHIVE_LABEL"
        echo
        echo "=== OS ==="
        grep -E '^(NAME|VERSION)=' /etc/os-release || true
        echo "Kernel: $(uname -r)"
        echo
        echo "=== Compiler ==="
        gcc --version | head -n1
        echo
        echo "=== Bound CPU: $CPU_PIN ==="
        lscpu | grep -E '^(Model name|CPU\(s\)|Thread|Core|Socket|CPU max MHz|CPU min MHz)' || true
        echo
        if [[ -r "/sys/devices/system/cpu/cpu${CPU_PIN}/cpufreq/scaling_governor" ]]; then
            echo "Governor:  $(cat /sys/devices/system/cpu/cpu${CPU_PIN}/cpufreq/scaling_governor)"
        fi
        echo
        echo "=== Memory ==="
        grep -E '^(MemTotal|MemFree|MemAvailable):' /proc/meminfo
        echo
        echo "=== tuned ==="
        tuned-adm active 2>/dev/null || echo "tuned not running"
        echo
        echo "=== Run command ==="
        echo "taskset -c $CPU_PIN $BENCH_BIN ${BENCH_ARGS[*]}"
    } > "$OUT_ENV"

    echo ">>> Archive target: $OUT_JSON"
    echo ">>> Env report:     $OUT_ENV"
fi

# ---------- sanity hint (not enforced) ----------
if [[ -r "/sys/devices/system/cpu/cpu${CPU_PIN}/cpufreq/scaling_governor" ]]; then
    GOV="$(cat /sys/devices/system/cpu/cpu${CPU_PIN}/cpufreq/scaling_governor)"
    if [[ "$GOV" != "performance" ]]; then
        echo "[hint] CPU governor is '$GOV'. For most stable numbers:"
        echo "       sudo cpupower frequency-set -g performance"
    fi
fi

# ---------- 裸设备依赖提示 ----------
# Cabe 是裸块设备引擎,Engine bench 需要 CABE_BENCH_DEVICE 指向真实块设备。
# 未设置时 Engine bench 会自动 SkipWithError,但 micro bench(crc32 / freelist /
# meta_index / chunk_index / buffer_pool)仍会正常跑。
if [[ -z "${CABE_BENCH_DEVICE:-}" ]]; then
    echo ""
    echo "[hint] CABE_BENCH_DEVICE not set. Engine end-to-end bench will be SKIPPED."
    echo "       To enable them (recommend >= 16 GiB device for full BM_Engine_Put coverage):"
    echo "         sudo SIZE_MB=16384 ./scripts/mkloop.sh create"
    echo "         export CABE_BENCH_DEVICE=/dev/loopX  # see mkloop output"
    echo "         ./scripts/run-bench.sh"
    echo ""
fi

# ---------- run with CPU pinning ----------
echo "=== Running on CPU $CPU_PIN ==="
taskset -c "$CPU_PIN" "$BENCH_BIN" "${BENCH_ARGS[@]}"