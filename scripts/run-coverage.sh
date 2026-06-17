#!/usr/bin/env bash
# ============================================================
# Cabe —— 覆盖率脚本（P0-M6；P4-M1 扩展 --backend / --index）
# 与 run-tests.sh 分离：覆盖率插桩开关 (-DCABE_COVERAGE=ON) 与
# 检测器同开会污染报告，故独立 build dir、独立流程。
# 设计依据：doc/P0/P0M6_test_scripts_design.md §5
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    cat <<'EOF'
用法: scripts/run-coverage.sh [选项]

选项:
  --compiler=NAME   工具链（g++ / clang++，默认 g++）
                    覆盖率工具自动对应：g++ → gcovr；clang++ → llvm-cov
  --backend=NAME    IoBackend 选择（必填）: sync | io_uring | spdk
                    自 P6 起取消默认后端、必须显式指定（见 doc/P6/README.md D10）
  --index=NAME      MetaIndex 选择: hashmap（默认）| bplustree
  --device=PATH           数据设备路径（如 /dev/loop0）
  --wal-device=PATH       WAL 设备路径（如 /dev/loop1）
  --snapshot-device=PATH  快照设备路径（如 /dev/loop2）
                          需设备的测试要三者齐备（./scripts/mkloop.sh create 一键创建）
  --strict          总行覆盖率 <80% 时退出码 1
                    （默认仅打印 + 着色提示，不阻塞）
  -h, --help        输出本用法

示例:
  ./scripts/run-coverage.sh --strict                                  (sync + hashmap，无设备测试跳过)
  ./scripts/run-coverage.sh --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --strict

退出码:
  0  覆盖率脚本跑完（无 --strict 时；或 --strict 且 ≥80%）
  1  --strict 且总覆盖率 <80%（或无法解析数值）
  2  参数错误
  3  依赖缺失（gcovr / llvm-cov / llvm-profdata）
  4  构建 / ctest 失败
EOF
}

# ---------- 默认 ----------
COMPILER=g++
BACKEND=""          # P6M3-D16：无默认后端，必须经 --backend 显式传入
INDEX=hashmap
DEVICE=""
WAL_DEVICE=""
SNAPSHOT_DEVICE=""
STRICT=false

# ---------- 参数解析 ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --compiler=*)
            v="${1#*=}"
            case "$v" in
                g++|clang++) COMPILER="$v" ;;
                *) echo "Error: --compiler= 仅接受 g++ / clang++（got: $v）" >&2; exit 2 ;;
            esac
            ;;
        --backend=*)
            v="${1#*=}"
            case "$v" in
                sync|io_uring|spdk) BACKEND="$v" ;;
                *) echo "Error: --backend= 仅接受 sync / io_uring / spdk（got: $v）" >&2; exit 2 ;;
            esac
            ;;
        --index=*)
            v="${1#*=}"
            case "$v" in
                hashmap|bplustree) INDEX="$v" ;;
                *) echo "Error: --index= 仅接受 hashmap / bplustree（got: $v）" >&2; exit 2 ;;
            esac
            ;;
        --device=*)
            DEVICE="${1#*=}"
            ;;
        --wal-device=*)
            WAL_DEVICE="${1#*=}"
            ;;
        --snapshot-device=*)
            SNAPSHOT_DEVICE="${1#*=}"
            ;;
        --strict)  STRICT=true ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Error: 未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---------- 后端必填校验（P6M3-D16：自 P6 起取消默认后端） ----------
if [[ -z "$BACKEND" ]]; then
    echo "Error: --backend 为必填项（自 P6 起取消默认后端，见 doc/P6/README.md D10）。" >&2
    echo "  指定其一: --backend=sync | --backend=io_uring" >&2
    echo "  例: ./scripts/run-coverage.sh --backend=sync --device=... --wal-device=... --snapshot-device=... --strict" >&2
    exit 2
fi

# ---------- 颜色 ----------
if [[ -t 1 ]]; then
    C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_YELLOW=$'\033[33m'; C_RST=$'\033[0m'
else
    C_GREEN=""; C_RED=""; C_YELLOW=""; C_RST=""
fi

# ---------- 依赖自检（M6-D8：gcovr 在 setup-dev.sh 已加，正常环境不触发） ----------
need_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: 缺少 '$1'。安装：$2" >&2
        exit 3
    fi
}
case "$COMPILER" in
    g++)
        need_tool gcovr "sudo dnf install gcovr"
        ;;
    clang++)
        need_tool llvm-cov      "sudo dnf install llvm"
        need_tool llvm-profdata "sudo dnf install llvm"
        need_tool jq            "sudo dnf install jq"
        ;;
esac

# ---------- build dir（D4：源码树内、跑前清空、跑完不清） ----------
if [[ "$COMPILER" == "g++" ]]; then
    compiler_short=gcc
else
    compiler_short=clang
fi
dir_suffix="${compiler_short}-coverage"
if [[ "$BACKEND" != "sync" ]]; then
    dir_suffix="${dir_suffix}-${BACKEND}"
fi
if [[ "$INDEX" != "hashmap" ]]; then
    dir_suffix="${dir_suffix}-${INDEX}"
fi
BUILD_DIR="$ROOT/build-tests/${dir_suffix}"

echo ">>> 覆盖率：compiler=$COMPILER  backend=$BACKEND  index=$INDEX  device=${DEVICE:-（未指定）}  wal=${WAL_DEVICE:-（未指定）}  snapshot=${SNAPSHOT_DEVICE:-（未指定）}  build_dir=$BUILD_DIR"

# rm/mkdir 失败硬卡：拒绝在脏目录上继续，避免旧 .gcda / .profraw 污染本次覆盖率数值
rm -rf "$BUILD_DIR"  || { echo "Error: rm -rf $BUILD_DIR 失败（权限或磁盘？）" >&2; exit 4; }
mkdir -p "$BUILD_DIR" || { echo "Error: mkdir $BUILD_DIR 失败" >&2; exit 4; }

# ---------- 配置 + 构建 ----------
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_CXX_COMPILER="$COMPILER" \
    -DCABE_BUILD_TESTS=ON \
    -DCABE_COVERAGE=ON \
    -DCABE_IO_BACKEND="$BACKEND" \
    -DCABE_META_INDEX="$INDEX" \
    || { echo "Error: cmake configure 失败" >&2; exit 4; }

cmake --build "$BUILD_DIR" \
    || { echo "Error: 构建失败" >&2; exit 4; }

# ---------- 跑 ctest ----------
# Clang 路径：每进程独立 profraw 文件（%p=pid, %m=binary-hash），否则后跑覆盖前跑
if [[ "$COMPILER" == "clang++" ]]; then
    mkdir -p "$BUILD_DIR/cov"
    export LLVM_PROFILE_FILE="$BUILD_DIR/cov/%p-%m.profraw"
fi

[[ -n "$DEVICE" ]]          && export CABE_TEST_DEVICE="$DEVICE"
[[ -n "$WAL_DEVICE" ]]      && export CABE_TEST_WAL_DEVICE="$WAL_DEVICE"
[[ -n "$SNAPSHOT_DEVICE" ]] && export CABE_TEST_SNAPSHOT_DEVICE="$SNAPSHOT_DEVICE"

ctest --test-dir "$BUILD_DIR" --output-on-failure \
    || { echo "Error: ctest 失败" >&2; exit 4; }

# ---------- 生成报告 ----------
echo
total_pct=""
case "$COMPILER" in
    g++)
        echo "==== 覆盖率（g++ + gcovr | backend=$BACKEND index=$INDEX）===="
        # --filter 限定 util/ + common/ + engine/ + io/ + index/
        # --exclude 把 *_test.cpp 兜底排除
        # --exclude logger.h：纯头宏最简实现
        # --gcov-ignore-parse-errors=negative_hits.warn_once_per_file：
        #   gcov 15.x 对头文件里的 inline constexpr（如 options.h 的 IsWalSyncLevel）
        #   偶发负计数（GCC bug #68080：多 TU .gcda 合并下溢）。gcovr 默认把负计数当致命错、
        #   一处就废掉整份报告；此选项降级为「按字面值收下、每文件警告一次」，不阻断报告。
        report=$(gcovr -r "$ROOT" \
            --filter "${ROOT}/util/" \
            --filter "${ROOT}/common/" \
            --filter "${ROOT}/engine/" \
            --filter "${ROOT}/io/" \
            --filter "${ROOT}/index/" \
            --exclude '.*_test\.cpp' \
            --exclude '.*logger\.h' \
            --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
            --print-summary \
            "$BUILD_DIR" 2>&1) || { echo "Error: gcovr 失败" >&2; echo "$report" >&2; exit 4; }
        echo "$report"
        total_pct=$(awk '/^lines:/ { gsub("%",""); print $2; exit }' <<< "$report")
        ;;
    clang++)
        echo "==== 覆盖率（clang++ + llvm-cov | backend=$BACKEND index=$INDEX）===="
        shopt -s nullglob
        raws=("$BUILD_DIR/cov"/*.profraw)
        shopt -u nullglob
        if (( ${#raws[@]} == 0 )); then
            echo "Error: 未找到 *.profraw（LLVM_PROFILE_FILE 未生效？）" >&2
            exit 4
        fi
        llvm-profdata merge -sparse "${raws[@]}" -o "$BUILD_DIR/cov/merged.profdata" \
            || { echo "Error: llvm-profdata merge 失败" >&2; exit 4; }

        shopt -s nullglob
        all_test_files=("$BUILD_DIR"/test/test_*)
        shopt -u nullglob
        bins=()
        for f in "${all_test_files[@]}"; do
            [[ -f "$f" && -x "$f" && ! "$f" =~ \. ]] && bins+=("$f")
        done
        if (( ${#bins[@]} == 0 )); then
            echo "Error: 未找到 test_* 可执行二进制 于 $BUILD_DIR/test/" >&2
            exit 4
        fi
        # 多二进制汇总：第一个直接传，其余用 -object=
        obj_args=()
        for ((i=1; i<${#bins[@]}; i++)); do obj_args+=("-object=${bins[$i]}"); done

        # 详细报告（按文件）
        llvm-cov report \
            --instr-profile="$BUILD_DIR/cov/merged.profdata" \
            "${bins[0]}" "${obj_args[@]}" \
            "$ROOT/util/" "$ROOT/common/" "$ROOT/engine/" "$ROOT/io/" "$ROOT/index/" \
            -ignore-filename-regex='(_test\.cpp|logger\.h)$' \
            || { echo "Error: llvm-cov report 失败" >&2; exit 4; }

        # 用 export(JSON) 解析总行覆盖率
        json="$BUILD_DIR/cov/coverage.json"
        llvm-cov export \
            --instr-profile="$BUILD_DIR/cov/merged.profdata" \
            "${bins[0]}" "${obj_args[@]}" \
            "$ROOT/util/" "$ROOT/common/" "$ROOT/engine/" "$ROOT/io/" "$ROOT/index/" \
            -ignore-filename-regex='(_test\.cpp|logger\.h)$' > "$json" \
            || { echo "Error: llvm-cov export 失败" >&2; exit 4; }
        total_pct=$(jq -r '.data[0].totals.lines.percent // empty' "$json")
        ;;
esac

# ---------- 80% 门槛 ----------
echo "--------------"
if [[ -z "$total_pct" ]]; then
    printf '%s警告：未从工具输出解析出总行覆盖率，请人工核对上方报告%s\n' "$C_YELLOW" "$C_RST"
    $STRICT && { printf '%s--strict 下解析失败按硬卡处理%s\n' "$C_RED" "$C_RST"; exit 1; }
    exit 0
fi

if awk -v p="$total_pct" 'BEGIN { exit !(p+0 >= 80.0) }'; then
    printf '总行覆盖率: %s%s%%%s   >= 80%% %s✓%s\n' \
        "$C_GREEN" "$total_pct" "$C_RST" "$C_GREEN" "$C_RST"
    exit 0
else
    printf '总行覆盖率: %s%s%%%s   <  80%% %s✗%s\n' \
        "$C_YELLOW" "$total_pct" "$C_RST" "$C_YELLOW" "$C_RST"
    if $STRICT; then
        printf '%s--strict 硬卡：退出码 1%s\n' "$C_RED" "$C_RST"
        exit 1
    fi
    exit 0
fi
