#!/usr/bin/env bash
# ============================================================
# 创建 / 清理 / 查询 loop 设备（三块：数据 + WAL + 快照），供 Cabe 本地测试使用。
# P5 起 cabe 一个设备组 = 数据设备 + WAL 设备 + 快照设备，故测试需三块设备。
#
# 用法:
#   ./scripts/mkloop.sh create    # 创建三块 loop 设备
#   ./scripts/mkloop.sh cleanup   # 卸载 + 删除三个镜像
#   ./scripts/mkloop.sh status    # 查看当前状态
#
# 环境变量可覆盖:
#   DATA_SIZE_MB      数据设备镜像大小，默认 64
#   WAL_SIZE_MB       WAL 设备镜像大小，默认 16
#   SNAPSHOT_SIZE_MB  快照设备镜像大小，默认 32（约 WAL 两倍）
#   IMG_DIR           镜像目录，默认 /var/tmp
# ============================================================
set -euo pipefail

DATA_SIZE_MB="${DATA_SIZE_MB:-64}"
WAL_SIZE_MB="${WAL_SIZE_MB:-16}"
SNAPSHOT_SIZE_MB="${SNAPSHOT_SIZE_MB:-32}"
IMG_DIR="${IMG_DIR:-/var/tmp}"
ACTION="${1:-status}"

SUDO=""
[[ $EUID -ne 0 ]] && SUDO="sudo"

DATA_IMG="$IMG_DIR/cabe_test_data.img"
WAL_IMG="$IMG_DIR/cabe_test_wal.img"
SNAPSHOT_IMG="$IMG_DIR/cabe_test_snapshot.img"

find_loop() {  # $1 = 镜像路径；按 backing file 精确匹配该镜像绑定的 loop 设备（首个）
    # 用 losetup -j 精确按 backing file 匹配，避免把路径当正则/子串误配其它镜像
    $SUDO losetup -j "$1" 2>/dev/null | head -n1 | cut -d: -f1 || true
}

create_one() {  # $1=镜像路径 $2=大小MB；输出 loop 设备路径到 stdout
    local img="$1" size="$2" dev
    if [[ ! -f "$img" ]]; then
        if command -v fallocate &>/dev/null; then
            $SUDO fallocate -l "${size}M" "$img"
        else
            $SUDO dd if=/dev/zero of="$img" bs=1M count="$size" status=none
        fi
        $SUDO chmod 644 "$img"
    fi
    dev=$(find_loop "$img")
    if [[ -z "$dev" ]]; then
        dev=$($SUDO losetup --find --show "$img")
    fi
    $SUDO chmod 660 "$dev" || true
    echo "$dev"
}

case "$ACTION" in
    create)
        DATA_DEV=$(create_one "$DATA_IMG" "$DATA_SIZE_MB")
        WAL_DEV=$(create_one "$WAL_IMG" "$WAL_SIZE_MB")
        SNAPSHOT_DEV=$(create_one "$SNAPSHOT_IMG" "$SNAPSHOT_SIZE_MB")
        echo ""
        echo "  数据设备: $DATA_DEV ($DATA_SIZE_MB MiB)"
        echo "  WAL 设备: $WAL_DEV ($WAL_SIZE_MB MiB)"
        echo "  快照设备: $SNAPSHOT_DEV ($SNAPSHOT_SIZE_MB MiB)"
        echo ""
        echo "测试时使用（环境变量）:"
        echo "  export CABE_TEST_DEVICE=$DATA_DEV"
        echo "  export CABE_TEST_WAL_DEVICE=$WAL_DEV"
        echo "  export CABE_TEST_SNAPSHOT_DEVICE=$SNAPSHOT_DEV"
        echo ""
        echo "或直接传给测试脚本:"
        echo "  ./scripts/run-tests.sh --device=$DATA_DEV --wal-device=$WAL_DEV --snapshot-device=$SNAPSHOT_DEV"
        echo ""
        echo "清理: $0 cleanup"
        ;;
    cleanup)
        for img in "$DATA_IMG" "$WAL_IMG" "$SNAPSHOT_IMG"; do
            # 卸载该镜像绑定的全部 loop 设备（可能不止一个）
            while read -r dev; do
                [[ -z "$dev" ]] && continue
                echo ">>> 卸载 $dev"
                $SUDO losetup -d "$dev"
            done < <($SUDO losetup -j "$img" 2>/dev/null | cut -d: -f1)
            [[ -f "$img" ]] && $SUDO rm -f "$img" && echo ">>> 已删除 $img"
        done
        echo ">>> 清理完成"
        ;;
    status)
        for pair in "数据:$DATA_IMG" "WAL :$WAL_IMG" "快照:$SNAPSHOT_IMG"; do
            role="${pair%%:*}"; img="${pair#*:}"
            echo "$role 设备: $img"
            if [[ -f "$img" ]]; then
                size=$(stat -c%s "$img")
                echo "  存在: 是 ($((size / 1024 / 1024)) MiB)"
            else
                echo "  存在: 否"
            fi
            dev=$(find_loop "$img")
            if [[ -n "$dev" ]]; then
                echo "  挂载: $dev"
            else
                echo "  挂载: 否"
            fi
        done
        ;;
    *)
        sed -n '2,16p' "$0"; exit 1 ;;
esac
