#!/usr/bin/env bash
# ============================================================
# Cabe —— 微基准运行脚本（P0-M7）
# 跑 {g++, clang++} × Release 的 google-benchmark；可选 --baseline 归档
# 设计依据：doc/P0/P0M7_convergence_design.md §5
# ============================================================
# set -e 不开：单格失败要继续跑剩余格子并在末尾汇总
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    cat <<'EOF'
用法: scripts/run-bench.sh [选项]

工具链选项:
  --compiler=NAME   只在指定工具链上跑（g++ / clang++ / all，默认 all）

归档选项:
  --baseline=PATH   把跑出的中位数结果写到 JSON 文件（按 P0M7 §5.2 schema）
                    不传则仅 stdout 打印 google-benchmark 原始报告

冗余度:
  -v, --verbose     输出每格完整 cmake / build / 微基准日志
                    默认精简：每格一行 OK / FAIL，失败时 dump 该格 log

其他:
  -h, --help        输出本用法

退出码:
  0  全部 OK；若传 --baseline 则 JSON 写入成功
  1  任一格 FAIL；或 --baseline 写入失败
  2  参数错误
EOF
}

# ---------- 默认 ----------
RUN_COMPILERS=(g++ clang++)
VERBOSE=false
BASELINE_FILE=""

# ---------- 参数解析 ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --compiler=*)
            v="${1#*=}"
            case "$v" in
                g++)     RUN_COMPILERS=(g++) ;;
                clang++) RUN_COMPILERS=(clang++) ;;
                all)     RUN_COMPILERS=(g++ clang++) ;;
                *) echo "Error: --compiler= 仅接受 g++ / clang++ / all（got: $v）" >&2; exit 2 ;;
            esac
            ;;
        --baseline=*) BASELINE_FILE="${1#*=}" ;;
        -v|--verbose) VERBOSE=true ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "Error: 未知参数: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# ---------- 颜色 ----------
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

# ---------- baseline 前置依赖自检（jq 在归档环节用） ----------
if [[ -n "$BASELINE_FILE" ]]; then
    if ! command -v jq >/dev/null 2>&1; then
        echo "Error: --baseline 需要 'jq'。安装：sudo dnf install jq" >&2
        exit 1
    fi
fi

# ---------- 单格执行 ----------
# $1=compiler $2=idx $3=total
# 写入全局 RESULTS（格式："compiler|OK|build_dir" 或 "compiler|FAIL(stage)|build_dir"）
run_one() {
    local compiler="$1" idx="$2" total="$3"
    local compiler_short build_dir log
    case "$compiler" in
        g++)     compiler_short=gcc   ;;
        clang++) compiler_short=clang ;;
    esac
    build_dir="$ROOT/build-bench/${compiler_short}-release"
    log="$build_dir/run.log"

    # M6-D4 风格：跑前清空、跑完不清。失败硬卡，避免脏目录上跑出虚假数。
    if ! rm -rf "$build_dir" 2>/dev/null || ! mkdir -p "$build_dir" 2>/dev/null; then
        if $VERBOSE; then
            printf '\n%s==== [%d/%d] %s release ====%s\n' "$C_DIM" "$idx" "$total" "$compiler" "$C_RST" >&2
        else
            printf '  [%d/%d] %-7s %-7s ... ' "$idx" "$total" "$compiler" "release" >&2
        fi
        printf '%sFAIL (cleanup)%s\n' "$C_RED" "$C_RST" >&2
        echo "    (rm -rf / mkdir 失败 — 权限或磁盘问题；脚本拒绝在脏目录上继续)" >&2
        RESULTS+=("${compiler}|FAIL(cleanup)|${build_dir}")
        return
    fi

    local -a cfg_args=(
        -S "$ROOT" -B "$build_dir" -G Ninja
        -DCMAKE_CXX_COMPILER="$compiler"
        -DCMAKE_BUILD_TYPE=Release
        -DCABE_BUILD_BENCH=ON
    )

    if $VERBOSE; then
        printf '\n%s==== [%d/%d] %s release ====%s\n' "$C_DIM" "$idx" "$total" "$compiler" "$C_RST" >&2
        if ! cmake "${cfg_args[@]}" >&2; then
            RESULTS+=("${compiler}|FAIL(configure)|${build_dir}"); return
        fi
        if ! cmake --build "$build_dir" >&2; then
            RESULTS+=("${compiler}|FAIL(build)|${build_dir}"); return
        fi
        # 自动发现所有 bench_* 可执行（P1M5：新增 bench 不用改脚本）
        shopt -s nullglob
        local -a bins=("$build_dir"/bench/bench_*)
        shopt -u nullglob
        for bin in "${bins[@]}"; do
            [[ -x "$bin" ]] || continue
            local bname
            bname=$(basename "$bin")
            if ! "$bin" \
                    --benchmark_repetitions=5 \
                    --benchmark_report_aggregates_only=true \
                    --benchmark_format=json \
                    --benchmark_out="$build_dir/$bname.json" >&2; then
                RESULTS+=("${compiler}|FAIL($bname)|${build_dir}"); return
            fi
        done
        RESULTS+=("${compiler}|OK|${build_dir}")
        return
    fi

    # 精简模式：进度同行，失败时 dump 该格 log
    _dump_log() {
        sed 's/^/    /' "$log" >&2
        [[ -s "$log" ]] || echo "    (log 为空，可能 redirect 失败 / 磁盘满 / 进程被信号杀)" >&2
    }
    printf '  [%d/%d] %-7s %-7s ... ' "$idx" "$total" "$compiler" "release" >&2
    if ! cmake "${cfg_args[@]}" >"$log" 2>&1; then
        printf '%sFAIL (configure)%s\n' "$C_RED" "$C_RST" >&2; _dump_log
        RESULTS+=("${compiler}|FAIL(configure)|${build_dir}"); return
    fi
    if ! cmake --build "$build_dir" >>"$log" 2>&1; then
        printf '%sFAIL (build)%s\n' "$C_RED" "$C_RST" >&2; _dump_log
        RESULTS+=("${compiler}|FAIL(build)|${build_dir}"); return
    fi
    # 自动发现所有 bench_* 可执行
    shopt -s nullglob
    local -a bins=("$build_dir"/bench/bench_*)
    shopt -u nullglob
    for bin in "${bins[@]}"; do
        [[ -x "$bin" ]] || continue
        local bname
        bname=$(basename "$bin")
        if ! "$bin" \
                --benchmark_repetitions=5 \
                --benchmark_report_aggregates_only=true \
                --benchmark_format=json \
                --benchmark_out="$build_dir/$bname.json" >>"$log" 2>&1; then
            printf '%sFAIL (%s)%s\n' "$C_RED" "$bname" "$C_RST" >&2; _dump_log
            RESULTS+=("${compiler}|FAIL($bname)|${build_dir}"); return
        fi
    done
    printf '%sOK%s\n' "$C_GREEN" "$C_RST" >&2
    RESULTS+=("${compiler}|OK|${build_dir}")
}

# ---------- 主体 ----------
RESULTS=()
total=${#RUN_COMPILERS[@]}
echo ">>> 跑 $total 格: compilers=[${RUN_COMPILERS[*]}]  root=$ROOT" >&2
idx=0
for c in "${RUN_COMPILERS[@]}"; do
    idx=$((idx + 1))
    run_one "$c" "$idx" "$total"
done

# ---------- 汇总 ----------
echo
echo "==== 汇总 ===="
fail_count=0
for r in "${RESULTS[@]}"; do
    IFS='|' read -r c st dir <<< "$r"
    case "$st" in
        OK)
            printf '%-7s %-7s : %sOK%s\n' "$c" "RELEASE" "$S_GREEN" "$S_RST"
            ;;
        *)
            stage="${st#FAIL(}"; stage="${stage%)}"
            printf '%-7s %-7s : %sFAIL%s   stage=%s  build 目录: %s\n' \
                "$c" "RELEASE" "$S_RED" "$S_RST" "$stage" "$dir"
            fail_count=$((fail_count + 1))
            ;;
    esac
done
echo "--------------"
if (( fail_count == 0 )); then
    printf '%s全部 %d 格 OK%s\n' "$S_GREEN" "$total" "$S_RST"
else
    printf '%s%d / %d 失败%s\n' "$S_RED" "$fail_count" "$total" "$S_RST"
    exit 1
fi

# ---------- 若传 --baseline，合并 JSON 落入 ----------
if [[ -n "$BASELINE_FILE" ]]; then
    echo
    echo ">>> 归档基线: $BASELINE_FILE"
    mkdir -p "$(dirname "$BASELINE_FILE")"

    # 元数据
    git_commit=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
    captured_at=$(date +%Y-%m-%d)
    kernel=$(uname -r)
    cpu_model=$(grep -m1 'model name' /proc/cpuinfo | sed 's/^[^:]*:\s*//' || echo "unknown")
    gcc_ver=$(g++ --version 2>/dev/null | head -n1 || echo "n/a")
    clang_ver=$(clang++ --version 2>/dev/null | head -n1 || echo "n/a")
    has_sse42="false"
    grep -q sse4_2 /proc/cpuinfo && has_sse42="true"
    cpu_features=$(jq -n --argjson sse "$has_sse42" '[if $sse then "sse4.2" else empty end]')

    # 收集各工具链的 results（自动发现所有 bench_*.json）
    results_json='{}'
    for c in "${RUN_COMPILERS[@]}"; do
        case "$c" in
            g++)     compiler_short=gcc   ;;
            clang++) compiler_short=clang ;;
        esac
        build_dir="$ROOT/build-bench/${compiler_short}-release"

        shopt -s nullglob
        local -a json_files=("$build_dir"/bench_*.json)
        shopt -u nullglob
        if (( ${#json_files[@]} == 0 )); then
            echo "Warning: $c 无 bench JSON 文件，跳过" >&2
            continue
        fi

        # 合并所有 bench_*.json 的 benchmarks 数组，取 median 聚合
        compiler_results=$(jq -s '
            [.[].benchmarks[]]
            | map(select(.run_type == "aggregate" and .aggregate_name == "median"))
            | map({
                key: ("bench_" + (.name | sub("_median$"; "") | sub("^BM_"; "") | sub("^EngineBench/BM_"; "engine/BM_") | ascii_downcase)),
                value: {
                    items_per_second: (.items_per_second // null),
                    bytes_per_second: (.bytes_per_second // null),
                    cpu_time_ns: (.cpu_time // null)
                }
            })
            | from_entries
        ' "${json_files[@]}") || { echo "Error: jq 解析 $c 失败" >&2; exit 1; }
        results_json=$(echo "$results_json" | jq --arg c "$c" --argjson r "$compiler_results" '. + {($c): $r}')
    done

    # 拼接最终 JSON
    if ! jq -n \
        --argjson results "$results_json" \
        --arg captured_at "$captured_at" \
        --arg git_commit  "$git_commit" \
        --arg kernel      "$kernel" \
        --arg cpu_model   "$cpu_model" \
        --argjson cpu_features "$cpu_features" \
        --arg gcc_ver   "$gcc_ver" \
        --arg clang_ver "$clang_ver" \
        '{
            schema_version: "1.0",
            milestone: "P0M7",
            captured_at: $captured_at,
            git_commit: $git_commit,
            env: {
                kernel: $kernel,
                cpu_model: $cpu_model,
                cpu_features_relevant: $cpu_features
            },
            build: {
                type: "Release",
                cmake_flags: ["-DCABE_BUILD_BENCH=ON"],
                compiler_specific: {
                    "g++": $gcc_ver,
                    "clang++": $clang_ver
                }
            },
            method: {
                tool: "google-benchmark",
                repetitions: 5,
                aggregate: "median",
                rationale: "5 次重复取中位数：抑制单次波动 + 比均值更稳健于离群"
            },
            results: $results,
            notes: "P0 收敛基线；P1+ 不强制不退步——仅作回归参考。完整方法见 doc/P0/P0M7_convergence_design.md §5。"
        }' > "$BASELINE_FILE"
    then
        echo "Error: baseline JSON 写入失败" >&2
        exit 1
    fi

    # 再校验 jq 一次（输出 JSON 是否合法）
    if ! jq -e . "$BASELINE_FILE" >/dev/null; then
        echo "Error: 写出的 baseline JSON 不合法" >&2
        exit 1
    fi

    echo "基线已写入: $BASELINE_FILE"
fi

exit 0
