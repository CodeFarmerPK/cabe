#!/usr/bin/env bash
# ============================================================
# Cabe SPDK NVMe device helper.
#
# P9M0 scope:
#   - show read-only NVMe controller status
#   - bind exactly one explicit BDF for SPDK
#   - unbind exactly one explicit BDF back to kernel driver
#
# This script never auto-selects devices and never supports batch bind.
# ============================================================
set -euo pipefail
shopt -s nullglob

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPDK_DIR="$ROOT/third_party/spdk"
SPDK_SETUP="$SPDK_DIR/scripts/setup.sh"
SETUP_SPDK="$ROOT/scripts/setup-spdk.sh"

DEFAULT_DRIVER="vfio-pci"
SPDK_DRIVERS=("vfio-pci" "uio_pci_generic")
STATE_WAIT_ATTEMPTS=50
STATE_WAIT_INTERVAL_SECONDS=0.1

usage() {
    cat <<'EOF'
用法: scripts/spdk-device.sh <command> [选项]

命令:
  status [--bdf=BDF]                         只读展示 NVMe controller 状态
  bind --bdf=BDF [--driver=DRIVER] --confirm-bind
                                             显式接管单个 NVMe controller
  unbind --bdf=BDF --confirm-unbind          显式释放单个 NVMe controller

BDF 必须是完整 PCI 地址，例如:
  0000:13:00.0

驱动:
  默认: vfio-pci
  可显式指定: vfio-pci | uio_pci_generic

安全边界:
  - 不支持 bind-all、--all、多 BDF、--force。
  - bind/unbind 必须显式传入 --bdf 和确认参数。
  - bind 会拒绝系统盘、已挂载设备、有 holder 的设备和非 NVMe controller。
  - 默认 vfio-pci 要求目标设备存在 IOMMU group。
  - bind 不会修改已经配置的大页内存。
  - 脚本不会自动从 vfio-pci 降级到 uio_pci_generic。
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
    local bdf="$2"
    local driver="$3"
    shift 3

    echo "About to run privileged action:"
    echo "  action: $action"
    echo "  bdf:    $bdf"
    [[ -n "$driver" ]] && echo "  driver: $driver"
    print_command "$@"
    "$@"
}

normalize_bdf() {
    local bdf="$1"
    bdf="${bdf,,}"
    [[ "$bdf" =~ ^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$ ]] || die "invalid BDF: $1"
    printf '%s\n' "$bdf"
}

require_spdk_setup() {
    [[ -x "$SPDK_SETUP" ]] || die "SPDK setup.sh is missing or not executable; run: ./scripts/setup-spdk.sh init"
}

require_bdf_exists() {
    local bdf="$1"
    [[ -d "/sys/bus/pci/devices/$bdf" ]] || die "BDF does not exist: $bdf"
}

pci_class() {
    local bdf="$1"
    cat "/sys/bus/pci/devices/$bdf/class"
}

is_nvme_controller() {
    local class
    class="$(pci_class "$1")"
    [[ "${class:0:6}" == "0x0108" ]]
}

require_nvme_controller() {
    local bdf="$1"
    is_nvme_controller "$bdf" || die "$bdf is not an NVMe controller"
}

current_driver() {
    local bdf="$1"
    local link="/sys/bus/pci/devices/$bdf/driver"
    if [[ -L "$link" ]]; then
        basename "$(readlink -f "$link")"
    else
        printf 'none\n'
    fi
}

is_spdk_driver() {
    local driver="$1"
    [[ "$driver" == "vfio-pci" || "$driver" == "uio_pci_generic" ]]
}

driver_allowed() {
    local driver="$1"
    [[ "$driver" == "vfio-pci" || "$driver" == "uio_pci_generic" ]]
}

driver_available() {
    local driver="$1"
    [[ -d "/sys/bus/pci/drivers/$driver" || -d "/sys/module/${driver//-/_}" || -d "/sys/module/$driver" ]] && return 0
    if command -v modinfo >/dev/null 2>&1; then
        modinfo "${driver//-/_}" >/dev/null 2>&1 || modinfo "$driver" >/dev/null 2>&1
        return
    fi
    return 1
}

vendor_device_id() {
    local bdf="$1"
    local vendor device
    vendor="$(<"/sys/bus/pci/devices/$bdf/vendor")"
    device="$(<"/sys/bus/pci/devices/$bdf/device")"
    printf '%s %s\n' "$vendor" "$device"
}

iommu_group() {
    local bdf="$1"
    local path="/sys/bus/pci/devices/$bdf/iommu_group"
    if [[ -L "$path" ]]; then
        basename "$(readlink -f "$path")"
    else
        printf 'none\n'
    fi
}

block_devices_for_bdf() {
    local bdf="$1"
    local devs=()
    local path

    for path in /sys/block/nvme*n*; do
        [[ -e "$path" ]] || continue
        if [[ "$(readlink -f "$path")" == *"/$bdf/"* ]]; then
            devs+=("$(basename "$path")")
        fi
    done

    if ((${#devs[@]} > 0)); then
        printf '%s\n' "${devs[@]}" | sort -u
    fi
}

join_lines() {
    awk 'NF { if (out != "") out = out "," $0; else out = $0 } END { print out }'
}

device_size() {
    local dev="$1"
    lsblk -dn -o SIZE "/dev/$dev" 2>/dev/null | awk 'NF {print; exit}'
}

partitions_for_dev() {
    local dev="$1"
    lsblk -nr -o NAME,TYPE "/dev/$dev" 2>/dev/null | awk '$2 == "part" {print $1}' | join_lines
}

mountpoints_for_dev() {
    local dev="$1"
    lsblk -nr -o MOUNTPOINTS "/dev/$dev" 2>/dev/null | join_lines
}

holders_for_dev() {
    local dev="$1"
    local root="/sys/block/$dev"
    [[ -d "$root" ]] || return 0
    find -H "$root" -type l -path '*/holders/*' -printf '%f\n' 2>/dev/null | sort -u | join_lines
}

system_disk_from_mounts() {
    local mounts="$1"
    [[ "$mounts" =~ (^|,)/($|,) ||
       "$mounts" =~ (^|,)/boot($|,) ||
       "$mounts" =~ (^|,)/home($|,) ||
       "$mounts" =~ (^|,)/var($|,) ||
       "$mounts" =~ (^|,)\[SWAP\]($|,) ]]
}

collect_device_state() {
    local bdf="$1"
    local dev mounts holders parts sizes system_disk=false
    local block_devs=()
    mapfile -t block_devs < <(block_devices_for_bdf "$bdf")

    DEVICE_BLOCKS=""
    DEVICE_SIZES=""
    DEVICE_PARTITIONS=""
    DEVICE_MOUNTS=""
    DEVICE_HOLDERS=""
    DEVICE_SYSTEM_DISK="no"

    for dev in "${block_devs[@]}"; do
        DEVICE_BLOCKS+="${DEVICE_BLOCKS:+,}$dev"

        sizes="$(device_size "$dev")"
        [[ -n "$sizes" ]] && DEVICE_SIZES+="${DEVICE_SIZES:+,}$dev:$sizes"

        parts="$(partitions_for_dev "$dev")"
        [[ -n "$parts" ]] && DEVICE_PARTITIONS+="${DEVICE_PARTITIONS:+,}$parts"

        mounts="$(mountpoints_for_dev "$dev")"
        [[ -n "$mounts" ]] && DEVICE_MOUNTS+="${DEVICE_MOUNTS:+,}$mounts"

        holders="$(holders_for_dev "$dev")"
        [[ -n "$holders" ]] && DEVICE_HOLDERS+="${DEVICE_HOLDERS:+,}$holders"

        if system_disk_from_mounts "$mounts"; then
            system_disk=true
        fi
    done

    [[ -z "$DEVICE_BLOCKS" ]] && DEVICE_BLOCKS="none"
    [[ -z "$DEVICE_SIZES" ]] && DEVICE_SIZES="unknown"
    [[ -z "$DEVICE_PARTITIONS" ]] && DEVICE_PARTITIONS="none"
    [[ -z "$DEVICE_MOUNTS" ]] && DEVICE_MOUNTS="none"
    [[ -z "$DEVICE_HOLDERS" ]] && DEVICE_HOLDERS="none"
    if [[ "$system_disk" == "true" ]]; then
        DEVICE_SYSTEM_DISK="yes"
    fi
}

bind_eligibility_from_state() {
    local current_driver="$1"
    local target_driver="$2"
    local group="$3"

    if is_spdk_driver "$current_driver"; then
        printf 'already-bound|already bound to SPDK-compatible driver %s\n' "$current_driver"
    elif [[ "$DEVICE_SYSTEM_DISK" == "yes" ]]; then
        printf 'unsafe|device has system mountpoints\n'
    elif [[ "$DEVICE_MOUNTS" != "none" ]]; then
        printf 'unsafe|device has active mountpoints\n'
    elif [[ "$DEVICE_HOLDERS" != "none" ]]; then
        printf 'unsafe|device has holders\n'
    elif [[ "$DEVICE_BLOCKS" == "none" ]]; then
        printf 'unknown|no kernel block device mapping found\n'
    elif [[ "$target_driver" == "vfio-pci" && "$group" == "none" ]]; then
        printf 'unsafe|vfio-pci requires an IOMMU group; use a VM with virtual IOMMU or explicitly select uio_pci_generic\n'
    else
        printf 'safe|no mountpoint, no holder, not a system disk, and target driver requirements are satisfied\n'
    fi
}

print_status_one() {
    local bdf="$1"
    require_bdf_exists "$bdf"
    require_nvme_controller "$bdf"

    local driver ids group spdk_status elig reason
    driver="$(current_driver "$bdf")"
    ids="$(vendor_device_id "$bdf")"
    group="$(iommu_group "$bdf")"
    spdk_status="not-bound"
    is_spdk_driver "$driver" && spdk_status="bound"
    collect_device_state "$bdf"
    IFS='|' read -r elig reason < <(bind_eligibility_from_state "$driver" "$DEFAULT_DRIVER" "$group")

    echo "BDF:              $bdf"
    echo "PCI type:         NVMe controller"
    echo "Vendor/device:    $ids"
    echo "Driver:           $driver"
    echo "IOMMU group:      $group"
    echo "SPDK status:      $spdk_status"
    echo "Block devices:    $DEVICE_BLOCKS"
    echo "Size:             $DEVICE_SIZES"
    echo "Partitions:       $DEVICE_PARTITIONS"
    echo "Mountpoints:      $DEVICE_MOUNTS"
    echo "Holders:          $DEVICE_HOLDERS"
    echo "System disk:      $DEVICE_SYSTEM_DISK"
    echo "Eligibility driver: $DEFAULT_DRIVER"
    echo "Bind eligibility: $elig"
    echo "Reason:           $reason"
}

list_nvme_bdfs() {
    local path bdf
    for path in /sys/bus/pci/devices/*; do
        [[ -e "$path/class" ]] || continue
        bdf="$(basename "$path")"
        if is_nvme_controller "$bdf"; then
            printf '%s\n' "$bdf"
        fi
    done | sort
}

cmd_status() {
    local bdf="${1:-}"
    require_command lspci
    require_command lsblk
    require_spdk_setup

    if [[ -n "$bdf" ]]; then
        print_status_one "$bdf"
        return
    fi

    local any=false
    local item
    while IFS= read -r item; do
        any=true
        print_status_one "$item"
        echo ""
    done < <(list_nvme_bdfs)
    [[ "$any" == "true" ]] || die "no NVMe controllers found"
}

require_bind_safe() {
    local bdf="$1"
    local target_driver="$2"
    local current group elig reason

    require_bdf_exists "$bdf"
    require_nvme_controller "$bdf"
    driver_allowed "$target_driver" || die "unsupported driver: $target_driver"
    driver_available "$target_driver" || die "driver is not available: $target_driver"

    current="$(current_driver "$bdf")"
    if [[ "$current" == "$target_driver" ]]; then
        die "$bdf is already bound to $target_driver"
    fi
    if is_spdk_driver "$current"; then
        die "$bdf is already bound to SPDK-compatible driver $current; run explicit unbind first"
    fi

    group="$(iommu_group "$bdf")"
    collect_device_state "$bdf"
    IFS='|' read -r elig reason < <(bind_eligibility_from_state "$current" "$target_driver" "$group")
    [[ "$elig" == "safe" ]] || die "device is not safe to bind: $reason"
}

wait_for_driver() {
    local bdf="$1"
    local expected="$2"
    local attempt

    for ((attempt = 0; attempt < STATE_WAIT_ATTEMPTS; ++attempt)); do
        [[ "$(current_driver "$bdf")" == "$expected" ]] && return 0
        sleep "$STATE_WAIT_INTERVAL_SECONDS"
    done
    return 1
}

wait_for_block_mapping() {
    local bdf="$1"
    local attempt

    for ((attempt = 0; attempt < STATE_WAIT_ATTEMPTS; ++attempt)); do
        [[ -n "$(block_devices_for_bdf "$bdf")" ]] && return 0
        sleep "$STATE_WAIT_INTERVAL_SECONDS"
    done
    return 1
}

cmd_bind() {
    local bdf="$1"
    local driver="$2"

    "$SETUP_SPDK" check
    require_bind_safe "$bdf" "$driver"

    local sudo
    sudo="$(sudo_prefix)"
    if [[ -n "$sudo" ]]; then
        run_privileged "bind NVMe controller for SPDK" "$bdf" "$driver" "$sudo" env SKIP_HUGE=yes PCI_ALLOWED="$bdf" DRIVER_OVERRIDE="$driver" "$SPDK_SETUP"
    else
        run_privileged "bind NVMe controller for SPDK" "$bdf" "$driver" env SKIP_HUGE=yes PCI_ALLOWED="$bdf" DRIVER_OVERRIDE="$driver" "$SPDK_SETUP"
    fi

    wait_for_driver "$bdf" "$driver" || die "bind command returned but $bdf is not bound to $driver (current: $(current_driver "$bdf"))"
    echo "NVMe controller bound: $bdf -> $driver"
}

cmd_unbind() {
    local bdf="$1"
    require_spdk_setup
    require_bdf_exists "$bdf"
    require_nvme_controller "$bdf"

    local current
    current="$(current_driver "$bdf")"
    is_spdk_driver "$current" || die "$bdf is not bound to an SPDK-compatible driver (current: $current)"

    local sudo
    sudo="$(sudo_prefix)"
    if [[ -n "$sudo" ]]; then
        run_privileged "unbind NVMe controller from SPDK" "$bdf" "$current" "$sudo" env SKIP_HUGE=yes PCI_BLOCK_SYNC_ON_RESET=yes PCI_ALLOWED="$bdf" "$SPDK_SETUP" reset
    else
        run_privileged "unbind NVMe controller from SPDK" "$bdf" "$current" env SKIP_HUGE=yes PCI_BLOCK_SYNC_ON_RESET=yes PCI_ALLOWED="$bdf" "$SPDK_SETUP" reset
    fi

    wait_for_driver "$bdf" nvme || die "unbind command returned but $bdf did not return to nvme (current: $(current_driver "$bdf"))"
    wait_for_block_mapping "$bdf" || die "unbind restored nvme for $bdf but no kernel block device mapping appeared"
    echo "NVMe controller released: $bdf -> nvme"
}

parse_bdf_arg() {
    local value="$1"
    [[ -n "$value" ]] || die "--bdf requires a value"
    normalize_bdf "$value"
}

if [[ $# -eq 0 ]]; then
    usage
    exit 2
fi

cmd="$1"
shift

case "$cmd" in
    status)
        bdf=""
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --bdf=*) bdf="$(parse_bdf_arg "${1#*=}")" ;;
                --bdf)
                    [[ $# -ge 2 ]] || die "--bdf requires a value"
                    bdf="$(parse_bdf_arg "$2")"
                    shift
                    ;;
                --all|--force) die "$1 is not supported" ;;
                *) die "unknown status option: $1" ;;
            esac
            shift
        done
        cmd_status "$bdf"
        ;;
    bind)
        bdf=""
        driver="$DEFAULT_DRIVER"
        confirmed=false
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --bdf=*) bdf="$(parse_bdf_arg "${1#*=}")" ;;
                --bdf)
                    [[ $# -ge 2 ]] || die "--bdf requires a value"
                    bdf="$(parse_bdf_arg "$2")"
                    shift
                    ;;
                --driver=*) driver="${1#*=}" ;;
                --driver)
                    [[ $# -ge 2 ]] || die "--driver requires a value"
                    driver="$2"
                    shift
                    ;;
                --confirm-bind) confirmed=true ;;
                --all|--force|bind-all) die "$1 is not supported" ;;
                *) die "unknown bind option: $1" ;;
            esac
            shift
        done
        [[ -n "$bdf" ]] || die "bind requires --bdf"
        [[ "$confirmed" == "true" ]] || die "bind requires --confirm-bind"
        cmd_bind "$bdf" "$driver"
        ;;
    unbind)
        bdf=""
        confirmed=false
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --bdf=*) bdf="$(parse_bdf_arg "${1#*=}")" ;;
                --bdf)
                    [[ $# -ge 2 ]] || die "--bdf requires a value"
                    bdf="$(parse_bdf_arg "$2")"
                    shift
                    ;;
                --confirm-unbind) confirmed=true ;;
                --all|--force|bind-all) die "$1 is not supported" ;;
                *) die "unknown unbind option: $1" ;;
            esac
            shift
        done
        [[ -n "$bdf" ]] || die "unbind requires --bdf"
        [[ "$confirmed" == "true" ]] || die "unbind requires --confirm-unbind"
        cmd_unbind "$bdf"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        die "unknown command: $cmd"
        ;;
esac
