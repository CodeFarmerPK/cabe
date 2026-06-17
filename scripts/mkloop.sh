#!/usr/bin/env bash
# ============================================================
# 创建 / 清理 / 查询 loop 设备（三块：数据 + WAL + 快照），供 Cabe 本地测试使用。
# P5 起 cabe 一个设备组 = 数据设备 + WAL 设备 + 快照设备，故测试需三块设备。
#
# 用法:
#   ./scripts/mkloop.sh create        # 创建测试三设备（小，cabe_test_*.img）
#   ./scripts/mkloop.sh create-bench  # 创建性能基准三设备（大、稀疏，cabe_bench_*.img）
#   ./scripts/mkloop.sh cleanup       # 卸载 + 删除测试镜像
#   ./scripts/mkloop.sh cleanup-bench # 卸载 + 删除基准镜像
#   ./scripts/mkloop.sh status        # 查看当前状态
#
# 测试设备小、性能基准设备大且分开（P6M3）：测试套件靠小环测撞墙等用例，性能基准要大设备
# 避免写满 → 永不重开 → 稳态干净。两组镜像名不同，可并存、互不干扰。
#
# 环境变量可覆盖:
#   DATA_SIZE_MB      测试数据设备镜像大小，默认 64
#   WAL_SIZE_MB       测试 WAL 设备镜像大小，默认 16
#   SNAPSHOT_SIZE_MB  测试快照设备镜像大小，默认 32（约 WAL 两倍）
#   BENCH_DATA_SIZE_MB      基准数据设备，默认 32768（32 GiB，稀疏）
#   BENCH_WAL_SIZE_MB       基准 WAL 设备，默认 1024（1 GiB，稀疏；多线程微基准无回收需大环）
#   BENCH_SNAPSHOT_SIZE_MB  基准快照设备，默认 128（MiB，稀疏）
#   IMG_DIR           镜像目录，默认 /var/tmp
# ============================================================
set -euo pipefail

DATA_SIZE_MB="${DATA_SIZE_MB:-64}"
WAL_SIZE_MB="${WAL_SIZE_MB:-16}"
SNAPSHOT_SIZE_MB="${SNAPSHOT_SIZE_MB:-32}"
# P6M3：性能基准大设备（稀疏，sparse=1），与测试小设备分开。
BENCH_DATA_SIZE_MB="${BENCH_DATA_SIZE_MB:-32768}"
BENCH_WAL_SIZE_MB="${BENCH_WAL_SIZE_MB:-1024}"
BENCH_SNAPSHOT_SIZE_MB="${BENCH_SNAPSHOT_SIZE_MB:-128}"
IMG_DIR="${IMG_DIR:-/var/tmp}"
ACTION="${1:-status}"

SUDO=""
[[ $EUID -ne 0 ]] && SUDO="sudo"

DATA_IMG="$IMG_DIR/cabe_test_data.img"
WAL_IMG="$IMG_DIR/cabe_test_wal.img"
SNAPSHOT_IMG="$IMG_DIR/cabe_test_snapshot.img"
BENCH_DATA_IMG="$IMG_DIR/cabe_bench_data.img"
BENCH_WAL_IMG="$IMG_DIR/cabe_bench_wal.img"
BENCH_SNAPSHOT_IMG="$IMG_DIR/cabe_bench_snapshot.img"

find_loop() {  # $1 = 镜像路径；按 backing file 精确匹配该镜像绑定的 loop 设备（首个）
    # 用 losetup -j 精确按 backing file 匹配，避免把路径当正则/子串误配其它镜像
    $SUDO losetup -j "$1" 2>/dev/null | head -n1 | cut -d: -f1 || true
}

create_one() {  # $1=镜像路径 $2=大小MB $3=sparse(1=稀疏truncate,默认0)；输出 loop 设备路径到 stdout
    local img="$1" size="$2" sparse="${3:-0}" dev
    if [[ ! -f "$img" ]]; then
        if [[ "$sparse" == "1" ]]; then
            $SUDO truncate -s "${size}M" "$img"   # 稀疏：瞬时创建、不实占盘，仅写入时占用
        elif command -v fallocate &>/dev/null; then
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
    create-bench)
        DATA_DEV=$(create_one "$BENCH_DATA_IMG" "$BENCH_DATA_SIZE_MB" 1)
        WAL_DEV=$(create_one "$BENCH_WAL_IMG" "$BENCH_WAL_SIZE_MB" 1)
        SNAPSHOT_DEV=$(create_one "$BENCH_SNAPSHOT_IMG" "$BENCH_SNAPSHOT_SIZE_MB" 1)
        echo ""
        echo "  [基准] 数据设备: $DATA_DEV ($BENCH_DATA_SIZE_MB MiB, 稀疏)"
        echo "  [基准] WAL 设备: $WAL_DEV ($BENCH_WAL_SIZE_MB MiB, 稀疏)"
        echo "  [基准] 快照设备: $SNAPSHOT_DEV ($BENCH_SNAPSHOT_SIZE_MB MiB, 稀疏)"
        echo ""
        echo "性能基准时使用（环境变量，沿用 CABE_TEST_* 名）:"
        echo "  export CABE_TEST_DEVICE=$DATA_DEV"
        echo "  export CABE_TEST_WAL_DEVICE=$WAL_DEV"
        echo "  export CABE_TEST_SNAPSHOT_DEVICE=$SNAPSHOT_DEV"
        echo ""
        echo "  bench_engine 用三设备；bench_wal_concurrency 只用 WAL 设备。"
        echo "  每次基准写入 ≤ 20G（< 32G value，永不写满）。清理: $0 cleanup-bench"
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
    cleanup-bench)
        for img in "$BENCH_DATA_IMG" "$BENCH_WAL_IMG" "$BENCH_SNAPSHOT_IMG"; do
            while read -r dev; do
                [[ -z "$dev" ]] && continue
                echo ">>> 卸载 $dev"
                $SUDO losetup -d "$dev"
            done < <($SUDO losetup -j "$img" 2>/dev/null | cut -d: -f1)
            [[ -f "$img" ]] && $SUDO rm -f "$img" && echo ">>> 已删除 $img"
        done
        echo ">>> 基准设备清理完成"
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
