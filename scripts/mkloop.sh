#!/usr/bin/env bash
# ============================================================
# Create or remove a loop device for Cabe local testing.
#
# Usage:
#   ./scripts/mkloop.sh create    # create loop device
#   ./scripts/mkloop.sh cleanup   # detach + remove image
#   ./scripts/mkloop.sh status    # show current state
#
# Env overrides:
#   SIZE_MB   image size, default 512
#   IMG_PATH  backing file, default /var/tmp/cabe_test.img
# ============================================================
set -euo pipefail

SIZE_MB="${SIZE_MB:-512}"
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
            echo ">>> Creating ${SIZE_MB} MiB image: $IMG_PATH"
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
            echo ">>> Reusing existing loop: $LOOP_DEV"
        else
            LOOP_DEV=$($SUDO losetup --find --show "$IMG_PATH")
            echo ">>> Attached at: $LOOP_DEV"
        fi
        $SUDO chmod 660 "$LOOP_DEV" || true
        echo ""
        echo "  Loop device: $LOOP_DEV"
        echo "  Backed by:   $IMG_PATH ($SIZE_MB MiB)"
        echo ""
        echo "Use in tests:  export CABE_TEST_DEVICE=$LOOP_DEV"
        echo "Clean up:      $0 cleanup"
        ;;
    cleanup)
        loops=$(find_loops)
        if [[ -n "$loops" ]]; then
            while IFS= read -r dev; do
                [[ -z "$dev" ]] && continue
                echo ">>> Detaching $dev"
                $SUDO losetup -d "$dev"
            done <<< "$loops"
        fi
        [[ -f "$IMG_PATH" ]] && $SUDO rm -f "$IMG_PATH" && echo ">>> Removed $IMG_PATH"
        echo ">>> Cleanup done"
        ;;
    status)
        echo "IMG_PATH: $IMG_PATH"
        if [[ -f "$IMG_PATH" ]]; then
            size=$(stat -c%s "$IMG_PATH")
            echo "  exists: yes ($((size / 1024 / 1024)) MiB)"
        else
            echo "  exists: no"
        fi
        loops=$(find_loops)
        [[ -n "$loops" ]] && echo "  attached at:" && echo "$loops" | sed 's/^/    /' || echo "  attached: no"
        ;;
    *)
        sed -n '2,14p' "$0"; exit 1 ;;
esac