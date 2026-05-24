#!/usr/bin/env bash
# ============================================================
# Cabe —— 本地 sanitizer 矩阵测试脚本（P0-M6）
# 跑 {g++, clang++} × {ASAN, TSAN, UBSAN, Release} = 八格组合矩阵
# 设计依据：doc/P0/P0M6_test_scripts_design.md §4
#
# 注：当前 M6 阶段 io_uring 后端尚未接入（P4 才接）；
# P4 起本脚本需补 --backend={sync,io_uring,spdk} 参数，并阻断
# TSAN + io_uring 组合（与 CMake 层 FATAL_ERROR 配合，见 P0M6 §7）。
# ============================================================
# set -e 不开：单格失败要继续跑剩余格子并在末尾汇总
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    cat <<'EOF'
用法: scripts/run-tests.sh [选项]

档选项（互斥；多档同传则以最后一个为准；不传则默认 --all）:
  --asan            只跑 ASAN 档
  --tsan            只跑 TSAN 档
  --ubsan           只跑 UBSAN 档
  --release         只跑 Release 档
  --all             跑全部四档（默认）

工具链选项:
  --compiler=NAME   只在指定工具链上跑（g++ / clang++ / all，默认 all）

冗余度:
  -v, --verbose     输出每格完整日志（CMake configure + 构建 + ctest）
                    默认精简：每格一行 PASS/FAIL，失败时自动 dump 该格 log

其他:
  -h, --help        输出本用法

退出码:
  0  全格 PASS
  1  任一格 FAIL
  2  参数错误
EOF
}

# ---------- 默认 ----------
RUN_FLAVORS=(asan tsan ubsan release)
RUN_COMPILERS=(g++ clang++)
VERBOSE=false

# ---------- 参数解析 ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --asan)    RUN_FLAVORS=(asan)    ;;
        --tsan)    RUN_FLAVORS=(tsan)    ;;
        --ubsan)   RUN_FLAVORS=(ubsan)   ;;
        --release) RUN_FLAVORS=(release) ;;
        --all)     RUN_FLAVORS=(asan tsan ubsan release) ;;
        --compiler=*)
            v="${1#*=}"
            case "$v" in
                g++)     RUN_COMPILERS=(g++) ;;
                clang++) RUN_COMPILERS=(clang++) ;;
                all)     RUN_COMPILERS=(g++ clang++) ;;
                *) echo "Error: --compiler= 仅接受 g++ / clang++ / all（got: $v）" >&2; exit 2 ;;
            esac
            ;;
        -v|--verbose) VERBOSE=true ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "Error: 未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---------- 颜色（stderr 走进度；stdout 走汇总） ----------
if [[ -t 2 ]]; then
    C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_DIM=$'\033[2m'; C_RST=$'\033[0m'
else
    C_GREEN=""; C_RED=""; C_DIM=""; C_RST=""
fi
if [[ -t 1 ]]; then
    S_GREEN=$'\033[32m'; S_RED=$'\033[31m'; S_RST=$'\033[0m'
else
    S_GREEN=""; S_RED=""; S_RST=""
fi

# ---------- 单格执行 ----------
# $1=compiler $2=flavor $3=idx $4=total
# 写入全局 RESULTS（格式："compiler|flavor|PASS|build_dir" 或 "...|FAIL(stage)|..."）
run_one() {
    local compiler="$1" flavor="$2" idx="$3" total="$4"
    local compiler_short build_dir log
    case "$compiler" in
        g++)     compiler_short=gcc   ;;
        clang++) compiler_short=clang ;;
    esac
    build_dir="$ROOT/build-tests/${compiler_short}-${flavor}"
    log="$build_dir/run.log"

    local -a cfg_args=(
        -S "$ROOT" -B "$build_dir" -G Ninja
        -DCMAKE_CXX_COMPILER="$compiler"
        -DCABE_BUILD_TESTS=ON
    )
    case "$flavor" in
        asan)    cfg_args+=(-DCABE_SANITIZER=address   -DCMAKE_BUILD_TYPE=Debug) ;;
        tsan)    cfg_args+=(-DCABE_SANITIZER=thread    -DCMAKE_BUILD_TYPE=Debug) ;;
        ubsan)   cfg_args+=(-DCABE_SANITIZER=undefined -DCMAKE_BUILD_TYPE=Debug) ;;
        release) cfg_args+=(-DCMAKE_BUILD_TYPE=Release) ;;
    esac

    # M6-D4：跑前清空该格、跑完不清；只动自己这格。
    # rm/mkdir 失败硬卡：拒绝在脏目录上继续，避免半新半旧二进制造成伪 PASS。
    if ! rm -rf "$build_dir" 2>/dev/null || ! mkdir -p "$build_dir" 2>/dev/null; then
        if $VERBOSE; then
            printf '\n%s==== [%d/%d] %s %s ====%s\n' "$C_DIM" "$idx" "$total" "$compiler" "$flavor" "$C_RST" >&2
        else
            printf '  [%d/%d] %-7s %-7s ... ' "$idx" "$total" "$compiler" "$flavor" >&2
        fi
        printf '%sFAIL (cleanup)%s\n' "$C_RED" "$C_RST" >&2
        echo "    (rm -rf / mkdir 失败 — 权限或磁盘问题；脚本拒绝在脏目录上继续)" >&2
        RESULTS+=("${compiler}|${flavor}|FAIL(cleanup)|${build_dir}")
        return
    fi

    if $VERBOSE; then
        printf '\n%s==== [%d/%d] %s %s ====%s\n' "$C_DIM" "$idx" "$total" "$compiler" "$flavor" "$C_RST" >&2
        if ! cmake "${cfg_args[@]}" >&2; then
            RESULTS+=("${compiler}|${flavor}|FAIL(configure)|${build_dir}")
            return
        fi
        if ! cmake --build "$build_dir" >&2; then
            RESULTS+=("${compiler}|${flavor}|FAIL(build)|${build_dir}")
            return
        fi
        if ! ctest --test-dir "$build_dir" --output-on-failure >&2; then
            RESULTS+=("${compiler}|${flavor}|FAIL(ctest)|${build_dir}")
            return
        fi
        RESULTS+=("${compiler}|${flavor}|PASS|${build_dir}")
        return
    fi

    # 精简模式：进度同行，stage 失败时 dump 该格 log
    printf '  [%d/%d] %-7s %-7s ... ' "$idx" "$total" "$compiler" "$flavor" >&2
    # 通用：失败后 dump log；log 为空（redirect 失败 / 磁盘满）也给出提示，免得开发者摸黑找现场
    _dump_log() {
        sed 's/^/    /' "$log" >&2
        [[ -s "$log" ]] || echo "    (log 为空，可能 redirect 失败 / 磁盘满 / 进程被信号杀)" >&2
    }
    if ! cmake "${cfg_args[@]}" >"$log" 2>&1; then
        printf '%sFAIL (configure)%s\n' "$C_RED" "$C_RST" >&2
        _dump_log
        RESULTS+=("${compiler}|${flavor}|FAIL(configure)|${build_dir}")
        return
    fi
    if ! cmake --build "$build_dir" >>"$log" 2>&1; then
        printf '%sFAIL (build)%s\n' "$C_RED" "$C_RST" >&2
        _dump_log
        RESULTS+=("${compiler}|${flavor}|FAIL(build)|${build_dir}")
        return
    fi
    if ! ctest --test-dir "$build_dir" --output-on-failure >>"$log" 2>&1; then
        printf '%sFAIL (ctest)%s\n' "$C_RED" "$C_RST" >&2
        _dump_log
        RESULTS+=("${compiler}|${flavor}|FAIL(ctest)|${build_dir}")
        return
    fi
    printf '%sPASS%s\n' "$C_GREEN" "$C_RST" >&2
    RESULTS+=("${compiler}|${flavor}|PASS|${build_dir}")
}

# ---------- 主体 ----------
RESULTS=()
total=$(( ${#RUN_COMPILERS[@]} * ${#RUN_FLAVORS[@]} ))
echo ">>> 跑 $total 格: compilers=[${RUN_COMPILERS[*]}] flavors=[${RUN_FLAVORS[*]}]  root=$ROOT" >&2
idx=0
for c in "${RUN_COMPILERS[@]}"; do
    for f in "${RUN_FLAVORS[@]}"; do
        idx=$((idx + 1))
        run_one "$c" "$f" "$idx" "$total"
    done
done

# ---------- 汇总（P0M6 §4.3 格式） ----------
echo
echo "==== 汇总 ===="
fail_count=0
for r in "${RESULTS[@]}"; do
    IFS='|' read -r c f st dir <<< "$r"
    fu="${f^^}"                                   # asan -> ASAN
    case "$st" in
        PASS)
            printf '%-7s %-8s : %sPASS%s\n' "$c" "$fu" "$S_GREEN" "$S_RST"
            ;;
        *)
            stage="${st#FAIL(}"; stage="${stage%)}"   # "FAIL(build)" -> "build"
            printf '%-7s %-8s : %sFAIL%s   stage=%s  build 目录: %s\n' \
                "$c" "$fu" "$S_RED" "$S_RST" "$stage" "$dir"
            fail_count=$((fail_count + 1))
            ;;
    esac
done
echo "--------------"
if (( fail_count == 0 )); then
    printf '%s全部 %d 格 PASS%s\n' "$S_GREEN" "$total" "$S_RST"
    exit 0
else
    printf '%s%d / %d 失败%s\n' "$S_RED" "$fail_count" "$total" "$S_RST"
    exit 1
fi
