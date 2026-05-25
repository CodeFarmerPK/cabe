#!/usr/bin/env bash
# ============================================================
# 创建 / 清理 / 查询 loop 设备，供 Cabe 本地测试使用。
#
# 用法:
#   ./scripts/mkloop.sh create    # 创建 loop 设备
#   ./scripts/mkloop.sh cleanup   # 卸载 + 删除镜像
#   ./scripts/mkloop.sh status    # 查看当前状态
#
# 环境变量可覆盖:
#   SIZE_MB   镜像大小，默认 131072（128 GiB）
#   IMG_PATH  镜像文件路径，默认 /var/tmp/cabe_test.img
# ============================================================
set -euo pipefail

SIZE_MB="${SIZE_MB:-131072}"
IMG_PATH="${IMG_PATH:-/var/tmp/cabe_test.img}"
ACTION="${1:-status}"

SUDO=""
[[ $EUID -ne 0 ]] && SUDO="sudo"

find_loops() {
    $SUDO losetup -a 2>/dev/null | awk -F: -v img="$IMG_PATH" '$0 ~ img { print $1 }' || true
}

case "$ACTION" in
    create)
        if [[ ! -f "$IMG_PATH" ]]; then
            echo ">>> 创建 ${SIZE_MB} MiB 镜像: $IMG_PATH"
            if command -v fallocate &>/dev/null; then
                $SUDO fallocate -l "${SIZE_MB}M" "$IMG_PATH"
            else
                $SUDO dd if=/dev/zero of="$IMG_PATH" bs=1M count="$SIZE_MB" status=none
            fi
            $SUDO chmod 644 "$IMG_PATH"
        fi
        existing=$(find_loops)
        if [[ -n "$existing" ]]; then
            LOOP_DEV="$(echo "$existing" | head -n1)"
            echo ">>> 复用已有 loop: $LOOP_DEV"
        else
            LOOP_DEV=$($SUDO losetup --find --show "$IMG_PATH")
            echo ">>> 已挂载: $LOOP_DEV"
        fi
        $SUDO chmod 660 "$LOOP_DEV" || true
        echo ""
        echo "  Loop 设备: $LOOP_DEV"
        echo "  镜像文件:  $IMG_PATH ($SIZE_MB MiB)"
        echo ""
        echo "测试时使用:  export CABE_TEST_DEVICE=$LOOP_DEV"
        echo "清理:        $0 cleanup"
        ;;
    cleanup)
        loops=$(find_loops)
        if [[ -n "$loops" ]]; then
            while IFS= read -r dev; do
                [[ -z "$dev" ]] && continue
                echo ">>> 卸载 $dev"
                $SUDO losetup -d "$dev"
            done <<< "$loops"
        fi
        [[ -f "$IMG_PATH" ]] && $SUDO rm -f "$IMG_PATH" && echo ">>> 已删除 $IMG_PATH"
        echo ">>> 清理完成"
        ;;
    status)
        echo "IMG_PATH: $IMG_PATH"
        if [[ -f "$IMG_PATH" ]]; then
            size=$(stat -c%s "$IMG_PATH")
            echo "  存在: 是 ($((size / 1024 / 1024)) MiB)"
        else
            echo "  存在: 否"
        fi
        loops=$(find_loops)
        if [[ -n "$loops" ]]; then
            echo "  已挂载:"
            echo "$loops" | sed 's/^/    /'
        else
            echo "  已挂载: 否"
        fi
        ;;
    *)
        sed -n '2,14p' "$0"; exit 1 ;;
esac
