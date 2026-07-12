#!/usr/bin/env bash
# ============================================================
# Cabe SPDK environment bootstrap.
#
# P9M0 scope:
#   - manage third_party/spdk submodule at SPDK v26.01
#   - install SPDK dependencies via SPDK official scripts
#   - build SPDK with default configuration
#   - check version, build artifacts, and hugepage baseline
#   - explicitly configure hugepage memory when requested
#
# This script never binds or unbinds NVMe devices. Use
# scripts/spdk-device.sh for device operations.
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPDK_DIR="$ROOT/third_party/spdk"

SPDK_VERSION_TAG="v26.01"
SPDK_VERSION_FILE="26.01.0"
SPDK_LOCKED_COMMIT="2ef883ef96e79c3cc16da02f667a7a58c2453f2f"
SPDK_HUGEMEM_BASELINE_MIB=1024

usage() {
    cat <<EOF
用法: scripts/setup-spdk.sh <command> [选项]

命令:
  init                         初始化并校验 third_party/spdk 子模块
  deps                         调用 SPDK pkgdep.sh 安装构建依赖（需要 root/sudo）
  build [--jobs=N]             执行 SPDK 默认 ./configure && make -jN
  check                        校验版本、commit、构建产物、大页内存和基础工具
  hugepage --mem-mib=N --confirm
                               显式配置 SPDK 大页内存（需要 root/sudo）
                               P9M0 默认基线: ${SPDK_HUGEMEM_BASELINE_MIB} MiB
  all [--jobs=N]               init + deps + build + check
                               不配置 hugepage，不接管设备
  clean                        在 third_party/spdk 内执行 make clean

固定版本:
  tag:     ${SPDK_VERSION_TAG}
  version: ${SPDK_VERSION_FILE}
  commit:  ${SPDK_LOCKED_COMMIT}

安全边界:
  - 本脚本不绑定、不释放、不 reset 任何 NVMe 设备。
  - hugepage 只有在显式 hugepage --confirm 时才配置。
  - 任何失败都会直接中断，不自动修复、不静默跳过。
EOF
}

die() {
    echo "Error: $*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

sudo_prefix() {
    if [[ "${EUID}" -eq 0 ]]; then
        return 0
    fi
    require_command sudo
    printf '%s\n' sudo
}

print_command() {
    printf '  command:'
    printf ' %q' "$@"
    printf '\n'
}

run_privileged() {
    local action="$1"
    shift

    echo "About to run privileged action:"
    echo "  action: $action"
    print_command "$@"
    "$@"
}

require_spdk_dir() {
    [[ -d "$SPDK_DIR" ]] || die "third_party/spdk is missing; run: ./scripts/setup-spdk.sh init"
    [[ -x "$SPDK_DIR/scripts/setup.sh" ]] || die "SPDK setup.sh is missing or not executable"
    [[ -x "$SPDK_DIR/scripts/pkgdep.sh" ]] || die "SPDK pkgdep.sh is missing or not executable"
    [[ -x "$SPDK_DIR/configure" ]] || die "SPDK configure is missing or not executable"
}

check_spdk_version() {
    require_spdk_dir
    require_command git

    local head version submodule_status
    head="$(git -C "$SPDK_DIR" rev-parse HEAD)"
    [[ "$head" == "$SPDK_LOCKED_COMMIT" ]] || die "SPDK commit mismatch: got $head, expected $SPDK_LOCKED_COMMIT"

    [[ -f "$SPDK_DIR/VERSION" ]] || die "SPDK VERSION file is missing"
    version="$(tr -d '[:space:]' < "$SPDK_DIR/VERSION")"
    [[ "$version" == "$SPDK_VERSION_FILE" ]] || die "SPDK VERSION mismatch: got $version, expected $SPDK_VERSION_FILE"

    submodule_status="$(git -C "$SPDK_DIR" submodule status --recursive)"
    if grep -q '^-' <<< "$submodule_status"; then
        die "SPDK recursive submodules are not initialized; run: ./scripts/setup-spdk.sh init"
    fi
    if grep -Eq '^[+U]' <<< "$submodule_status"; then
        die "SPDK recursive submodules are not at their recorded commits; run: ./scripts/setup-spdk.sh init"
    fi
}

check_build_artifacts() {
    local required=(
        "$SPDK_DIR/build/bin/spdk_tgt"
        "$SPDK_DIR/build/bin/spdk_nvme_identify"
        "$SPDK_DIR/build/bin/spdk_nvme_perf"
        "$SPDK_DIR/build/examples/hello_world"
        "$SPDK_DIR/build/lib/libspdk_nvme.a"
    )
    local missing=()
    local path

    for path in "${required[@]}"; do
        [[ -e "$path" ]] || missing+=("${path#"$SPDK_DIR"/}")
    done

    ((${#missing[@]} == 0)) || die "SPDK build artifacts are missing: ${missing[*]}; run: ./scripts/setup-spdk.sh build"
}

check_hugepage() {
    [[ -r /proc/meminfo ]] || die "/proc/meminfo is not readable"
    [[ -d /dev/hugepages ]] || die "/dev/hugepages does not exist"
    require_command findmnt

    local mount_fstype hp_size_kb total free required_pages
    mount_fstype="$(findmnt -rn -T /dev/hugepages -o FSTYPE 2>/dev/null || true)"
    [[ "$mount_fstype" == "hugetlbfs" ]] || die "/dev/hugepages is not a hugetlbfs mount (got: ${mount_fstype:-none})"

    hp_size_kb="$(awk '/Hugepagesize:/ {print $2}' /proc/meminfo)"
    total="$(awk '/HugePages_Total:/ {print $2}' /proc/meminfo)"
    free="$(awk '/HugePages_Free:/ {print $2}' /proc/meminfo)"

    [[ -n "$hp_size_kb" && -n "$total" && -n "$free" ]] || die "failed to read hugepage counters from /proc/meminfo"
    [[ "$hp_size_kb" == "2048" ]] || die "P9M0 expects 2 MiB hugepages, got ${hp_size_kb} KiB"

    required_pages=$(( (SPDK_HUGEMEM_BASELINE_MIB * 1024 + hp_size_kb - 1) / hp_size_kb ))
    (( total >= required_pages )) || die "hugepage total too small: total=$total pages, required=$required_pages pages (${SPDK_HUGEMEM_BASELINE_MIB} MiB)"
    (( free >= required_pages )) || die "hugepage free too small: free=$free pages, required=$required_pages pages (${SPDK_HUGEMEM_BASELINE_MIB} MiB)"

    echo "hugepage size:   ${hp_size_kb} KiB"
    echo "hugepage total:  ${total}"
    echo "hugepage free:   ${free}"
    echo "hugepage status: ok"
}

cmd_init() {
    require_command git
    git -C "$ROOT" submodule update --init --recursive -- third_party/spdk
    check_spdk_version
    echo "SPDK submodule ready: ${SPDK_VERSION_TAG} (${SPDK_LOCKED_COMMIT})"
}

cmd_deps() {
    check_spdk_version
    local sudo
    sudo="$(sudo_prefix)"
    if [[ -n "$sudo" ]]; then
        run_privileged "install SPDK package dependencies" "$sudo" "$SPDK_DIR/scripts/pkgdep.sh"
    else
        run_privileged "install SPDK package dependencies" "$SPDK_DIR/scripts/pkgdep.sh"
    fi
}

cmd_build() {
    local jobs="$1"
    check_spdk_version
    require_command make

    (cd "$SPDK_DIR" && ./configure)
    (cd "$SPDK_DIR" && make -j"$jobs")
}

cmd_check() {
    check_spdk_version
    check_build_artifacts
    require_command lspci
    require_command lsblk
    check_hugepage
    echo "SPDK check OK"
}

cmd_hugepage() {
    local mem_mib="$1"
    check_spdk_version

    local sudo
    sudo="$(sudo_prefix)"
    if [[ -n "$sudo" ]]; then
        run_privileged "configure SPDK hugepage memory" "$sudo" env HUGEMEM="$mem_mib" PCI_ALLOWED=none "$SPDK_DIR/scripts/setup.sh"
    else
        run_privileged "configure SPDK hugepage memory" env HUGEMEM="$mem_mib" PCI_ALLOWED=none "$SPDK_DIR/scripts/setup.sh"
    fi
}

cmd_clean() {
    check_spdk_version
    require_command make
    (cd "$SPDK_DIR" && make clean)
}

parse_jobs() {
    local jobs
    jobs="$(nproc)"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --jobs=*) jobs="${1#*=}" ;;
            --jobs)
                [[ $# -ge 2 ]] || die "--jobs requires a value"
                jobs="$2"
                shift
                ;;
            *) die "unknown build option: $1" ;;
        esac
        shift
    done
    [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive integer"
    printf '%s\n' "$jobs"
}

parse_hugepage_mem() {
    local mem_mib="$SPDK_HUGEMEM_BASELINE_MIB"
    local confirmed=false
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --mem-mib=*) mem_mib="${1#*=}" ;;
            --mem-mib)
                [[ $# -ge 2 ]] || die "--mem-mib requires a value"
                mem_mib="$2"
                shift
                ;;
            --confirm) confirmed=true ;;
            *) die "unknown hugepage option: $1" ;;
        esac
        shift
    done
    [[ "$confirmed" == "true" ]] || die "hugepage requires explicit --confirm"
    [[ "$mem_mib" =~ ^[1-9][0-9]*$ ]] || die "--mem-mib must be a positive integer"
    printf '%s\n' "$mem_mib"
}

if [[ $# -eq 0 ]]; then
    usage
    exit 2
fi

cmd="$1"
shift

case "$cmd" in
    init)
        [[ $# -eq 0 ]] || die "init does not accept options"
        cmd_init
        ;;
    deps)
        [[ $# -eq 0 ]] || die "deps does not accept options"
        cmd_deps
        ;;
    build)
        jobs="$(parse_jobs "$@")"
        cmd_build "$jobs"
        ;;
    check)
        [[ $# -eq 0 ]] || die "check does not accept options"
        cmd_check
        ;;
    hugepage)
        mem_mib="$(parse_hugepage_mem "$@")"
        cmd_hugepage "$mem_mib"
        ;;
    all)
        jobs="$(parse_jobs "$@")"
        cmd_init
        cmd_deps
        cmd_build "$jobs"
        cmd_check
        ;;
    clean)
        [[ $# -eq 0 ]] || die "clean does not accept options"
        cmd_clean
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        die "unknown command: $cmd"
        ;;
esac
