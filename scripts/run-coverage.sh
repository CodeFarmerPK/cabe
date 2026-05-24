#!/usr/bin/env bash
# ============================================================
# Cabe —— util/ + common/ 覆盖率脚本（P0-M6）
# 与 run-tests.sh 分离：覆盖率插桩开关 (-DCABE_COVERAGE=ON) 与
# sanitizer 同开会污染报告，故独立 build dir、独立流程。
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
  --strict          util+common 总行覆盖率 <80% 时退出码 1
                    （默认仅打印 + 着色提示，不阻塞）
  -h, --help        输出本用法

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
        --strict)  STRICT=true ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Error: 未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

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
        ;;
esac

# ---------- build dir（D4：源码树内、跑前清空、跑完不清） ----------
[[ "$COMPILER" == "g++" ]] && compiler_short=gcc || compiler_short=clang
BUILD_DIR="$ROOT/build-tests/${compiler_short}-coverage"

echo ">>> 覆盖率：compiler=$COMPILER  build_dir=$BUILD_DIR"

# rm/mkdir 失败硬卡：拒绝在脏目录上继续，避免旧 .gcda / .profraw 污染本次覆盖率数值
rm -rf "$BUILD_DIR"  || { echo "Error: rm -rf $BUILD_DIR 失败（权限或磁盘？）" >&2; exit 4; }
mkdir -p "$BUILD_DIR" || { echo "Error: mkdir $BUILD_DIR 失败" >&2; exit 4; }

# ---------- 配置 + 构建 ----------
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_CXX_COMPILER="$COMPILER" \
    -DCABE_BUILD_TESTS=ON \
    -DCABE_COVERAGE=ON \
    || { echo "Error: cmake configure 失败" >&2; exit 4; }

cmake --build "$BUILD_DIR" \
    || { echo "Error: 构建失败" >&2; exit 4; }

# ---------- 跑 ctest ----------
# Clang 路径：每进程独立 profraw 文件（%p=pid, %m=binary-hash），否则后跑覆盖前跑
if [[ "$COMPILER" == "clang++" ]]; then
    mkdir -p "$BUILD_DIR/cov"
    export LLVM_PROFILE_FILE="$BUILD_DIR/cov/%p-%m.profraw"
fi

ctest --test-dir "$BUILD_DIR" --output-on-failure \
    || { echo "Error: ctest 失败" >&2; exit 4; }

# ---------- 生成报告 ----------
echo
total_pct=""
case "$COMPILER" in
    g++)
        echo "==== 覆盖率（g++ + gcovr）===="
        # --filter 限定 util/+common/；--exclude 把可能误入的 *_test.cpp 兜底排除
        # --print-summary 末尾追加 "lines: X% (a out of b)" 一行，便于解析
        report=$(gcovr -r "$ROOT" \
            --filter "${ROOT}/util/" \
            --filter "${ROOT}/common/" \
            --exclude '.*_test\.cpp' \
            --print-summary \
            "$BUILD_DIR" 2>&1) || { echo "Error: gcovr 失败" >&2; echo "$report" >&2; exit 4; }
        echo "$report"
        total_pct=$(awk '/^lines:/ { gsub("%",""); print $2; exit }' <<< "$report")
        ;;
    clang++)
        echo "==== 覆盖率（clang++ + llvm-cov）===="
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
        bins=("$BUILD_DIR"/test/test_*)
        shopt -u nullglob
        if (( ${#bins[@]} == 0 )); then
            echo "Error: 未找到 test_* 二进制 于 $BUILD_DIR/test/" >&2
            exit 4
        fi
        # 多二进制汇总：第一个直接传，其余用 -object=
        obj_args=()
        for ((i=1; i<${#bins[@]}; i++)); do obj_args+=("-object=${bins[$i]}"); done

        # 详细报告（按文件）
        llvm-cov report \
            --instr-profile="$BUILD_DIR/cov/merged.profdata" \
            "${bins[0]}" "${obj_args[@]}" \
            "$ROOT/util/" "$ROOT/common/" \
            -ignore-filename-regex='_test\.cpp$' \
            || { echo "Error: llvm-cov report 失败" >&2; exit 4; }

        # 用 export(JSON) 解析总行覆盖率（避免依赖 report 表格列位）
        json="$BUILD_DIR/cov/coverage.json"
        llvm-cov export \
            --instr-profile="$BUILD_DIR/cov/merged.profdata" \
            "${bins[0]}" "${obj_args[@]}" \
            "$ROOT/util/" "$ROOT/common/" \
            -ignore-filename-regex='_test\.cpp$' > "$json" \
            || { echo "Error: llvm-cov export 失败" >&2; exit 4; }
        # JSON 顶层 data[0].totals.lines.percent —— 用 grep 提取，避免 jq 依赖
        total_pct=$(grep -oE '"lines":\{[^}]*"percent":[0-9.]+' "$json" \
                    | tail -n1 | grep -oE '[0-9.]+$')
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
    printf 'util + common 总行覆盖率: %s%s%%%s   >= 80%% %s✓%s\n' \
        "$C_GREEN" "$total_pct" "$C_RST" "$C_GREEN" "$C_RST"
    exit 0
else
    printf 'util + common 总行覆盖率: %s%s%%%s   <  80%% %s✗%s\n' \
        "$C_YELLOW" "$total_pct" "$C_RST" "$C_YELLOW" "$C_RST"
    if $STRICT; then
        printf '%s--strict 硬卡：退出码 1%s\n' "$C_RED" "$C_RST"
        exit 1
    fi
    exit 0
fi
