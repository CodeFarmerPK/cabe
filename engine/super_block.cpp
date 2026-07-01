#include "engine/super_block.h"
#include "common/error_code.h"
#include "common/structs.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/raw_device.h"
#include "util/util.h"

#include <cerrno>
#include <cstring>
#include <sys/random.h>

namespace cabe {

    namespace {

        // 生成 128 位 UUID（P5M1-D3）。用 getrandom(2)（无 fd、阻塞至熵池就绪、失败即报错），
        // 失败返回 false 让 create 硬失败——绝不静默退化成低熵/零值：否则 UUID 碰撞会让 recover
        // 的配对/防漂移校验在错配设备上误通过而毁数据（安全攸关，与超级块校验同级别）。
        bool GenerateUuid(std::uint8_t out[kUuidBytes]) {
            std::size_t got = 0;
            while (got < kUuidBytes) {
                ssize_t n = ::getrandom(out + got, kUuidBytes - got, 0);
                if (n < 0) {
                    if (errno == EINTR) continue;  // 信号中断 → 重试
                    return false;
                }
                if (n == 0) return false;
                got += static_cast<std::size_t>(n);
            }
            return true;
        }

        std::uint32_t ComputeCrc(const SuperBlock& sb) {
            // 覆盖 [0, 4092)，即除末尾 crc32c 字段外的全部
            const auto* p = reinterpret_cast<const std::byte*>(&sb);
            return util::CRC32(DataView{p, kSuperBlockSize - sizeof(std::uint32_t)});
        }

        // 单份超级块校验结果：区分"非本格式/未格式化"与"本格式但已损坏"。
        enum class SbCheck { Valid, BadMagic, BadCrc };

        SbCheck CheckSuperBlock(const SuperBlock& sb) {
            // 魔数或版本不符 → 视为非本格式（含未格式化、插错盘、未来格式被旧二进制打开）。
            // 校验 version 可防 v2 超级块被 v1 二进制按错误语义读取而毁数据。
            if (sb.magic != kSuperBlockMagic) return SbCheck::BadMagic;
            if (sb.version != kSuperBlockVersion) return SbCheck::BadMagic;
            if (ComputeCrc(sb) != sb.crc32c) return SbCheck::BadCrc;
            return SbCheck::Valid;
        }

        // 写双份超级块并持久化。写序：先备份 @4K + Sync，再主份 @0 + Sync。
        // 不变量：主份只在完整备份已持久后才出现 → 任意断电点至少一份完整，且"主份有效 ⇒
        // 备份必有效"（与 ReadSuperBlockWithRepair 先主后备、主坏用备份修复天然契合）。据此
        // 撕裂的 create 自我失效，无需回滚——是对 D8（幂等 create、无两阶段提交）的细化。
        int32_t WriteSuperBlockPair(RawDevice& dev, const SuperBlock& sb) {
            std::byte* buf = RawDevice::AllocAligned(kSuperBlockSize);
            if (!buf) return err::kIoBase;
            std::memcpy(buf, &sb, kSuperBlockSize);
            int32_t rc = dev.WriteAt(kSuperBlockSize, buf, kSuperBlockSize);  // 备份先
            if (rc == err::kSuccess) rc = dev.Sync();
            if (rc == err::kSuccess) rc = dev.WriteAt(0, buf, kSuperBlockSize);  // 主份后
            if (rc == err::kSuccess) rc = dev.Sync();
            RawDevice::FreeAligned(buf);
            return rc;
        }

        // 读超级块：主份失败则读备份；主坏备好则用备份覆盖主份修复（P5M1-D4）。
        // 返回：kSuccess / kSuperBlockReadFailed（主备读 I/O 均失败，勿据此重建）/
        //       kSuperBlockMagicMismatch（读到但都非本格式）/ kSuperBlockCrcMismatch（本格式但都损坏）/
        //       kSuperBlockNoValid（兜底）。
        int32_t ReadSuperBlockWithRepair(RawDevice& dev, SuperBlock* out) {
            std::byte* buf = RawDevice::AllocAligned(kSuperBlockSize);
            if (!buf) return err::kIoBase;

            bool primary_read_ok = false, backup_read_ok = false;
            SbCheck primary_check = SbCheck::BadMagic;
            SbCheck backup_check  = SbCheck::BadMagic;

            // 主份 @0
            SuperBlock primary{};
            if (dev.ReadAt(0, buf, kSuperBlockSize) == err::kSuccess) {
                primary_read_ok = true;
                std::memcpy(&primary, buf, kSuperBlockSize);
                primary_check = CheckSuperBlock(primary);
                if (primary_check == SbCheck::Valid) {
                    *out = primary;
                    RawDevice::FreeAligned(buf);
                    return err::kSuccess;
                }
            }

            // 备份 @4K
            SuperBlock backup{};
            if (dev.ReadAt(kSuperBlockSize, buf, kSuperBlockSize) == err::kSuccess) {
                backup_read_ok = true;
                std::memcpy(&backup, buf, kSuperBlockSize);
                backup_check = CheckSuperBlock(backup);
                if (backup_check == SbCheck::Valid) {
                    // 用备份覆盖主份，恢复双份冗余（buf 已持有刚读出的备份字节）
                    if (dev.WriteAt(0, buf, kSuperBlockSize) != err::kSuccess) {
                        // 修复写失败：备份仍有效、本次 recover 成功，但冗余未恢复——必须可见
                        CABE_LOG_WARN("超级块主份修复写回失败，双份冗余未恢复（备份仍有效）");
                    } else {
                        (void)dev.Sync();  // 尽力持久化修复；失败不致 recover 失败（备份仍在）
                    }
                    *out = backup;
                    RawDevice::FreeAligned(buf);
                    return err::kSuccess;
                }
            }

            RawDevice::FreeAligned(buf);
            // 先区分"读 I/O 失败"与"读到了但内容无效"——避免把坏道/EIO 误报成"未格式化"，
            // 诱导运维重建而毁掉可恢复数据。
            if (!primary_read_ok && !backup_read_ok) return err::kSuperBlockReadFailed;
            if (primary_check == SbCheck::BadMagic && backup_check == SbCheck::BadMagic) {
                return err::kSuperBlockMagicMismatch;
            }
            if (primary_check == SbCheck::BadCrc || backup_check == SbCheck::BadCrc) {
                return err::kSuperBlockCrcMismatch;
            }
            return err::kSuperBlockNoValid;
        }

        bool UuidEqual(const std::uint8_t a[kUuidBytes], const std::uint8_t b[kUuidBytes]) {
            return std::memcmp(a, b, kUuidBytes) == 0;
        }

    } // namespace

    int32_t CreateDeviceGroup(const DeviceConfig& cfg, std::uint64_t device_id, std::uint64_t device_count, SuperBlock* out) {
        RawDevice data_dev, wal_dev, snap_dev;
        if (data_dev.Open(cfg.data_path) != err::kSuccess) return err::kIoBase;
        if (wal_dev.Open(cfg.wal_path) != err::kSuccess) return err::kIoBase;
        if (snap_dev.Open(cfg.snapshot_path) != err::kSuccess) return err::kIoBase;

        // 设备大小校验：数据设备 ≥ 头部 8K 超级块 + ≥1 数据块；WAL/快照 ≥ 头部 8K 超级块
        if (data_dev.SizeBytes() < kDataRegionOffset + kValueSize) return err::kEngineInvalidOpts;
        if (wal_dev.SizeBytes() < kDataRegionOffset) return err::kEngineInvalidOpts;
        if (snap_dev.SizeBytes() < kDataRegionOffset) return err::kEngineInvalidOpts;

        std::uint8_t engine_uuid[kUuidBytes], data_uuid[kUuidBytes],
                     wal_uuid[kUuidBytes], snap_uuid[kUuidBytes];
        if (!GenerateUuid(engine_uuid) || !GenerateUuid(data_uuid) ||
            !GenerateUuid(wal_uuid)    || !GenerateUuid(snap_uuid)) {
            CABE_LOG_ERROR("超级块 UUID 生成失败：系统熵源不可用，create 中止");
            return err::kSuperBlockEntropyFailure;
        }

        // 数据区从 kDataRegionOffset 起，逻辑 block 从 0 编号
        const std::uint64_t block_count = (data_dev.SizeBytes() - kDataRegionOffset) / kValueSize;
        const std::uint64_t now = util::GetWallTimeNs();

        // 数据设备超级块
        SuperBlock data_sb{};
        data_sb.magic = kSuperBlockMagic;
        data_sb.version = kSuperBlockVersion;
        data_sb.device_type = static_cast<std::uint32_t>(DeviceType::Data);
        std::memcpy(data_sb.engine_uuid, engine_uuid, kUuidBytes);
        std::memcpy(data_sb.device_uuid, data_uuid, kUuidBytes);
        std::memcpy(data_sb.paired_wal_uuid, wal_uuid, kUuidBytes);
        std::memcpy(data_sb.paired_snapshot_uuid, snap_uuid, kUuidBytes);
        data_sb.device_id = device_id;
        data_sb.block_count = block_count;
        data_sb.device_count = device_count;
        data_sb.created_at = now;
        data_sb.crc32c = ComputeCrc(data_sb);

        // WAL 超级块（block_count 仅数据设备有意义，WAL/快照留 0；各自容量在 M2/M4 定义）
        SuperBlock wal_sb{};
        wal_sb.magic = kSuperBlockMagic;
        wal_sb.version = kSuperBlockVersion;
        wal_sb.device_type = static_cast<std::uint32_t>(DeviceType::Wal);
        std::memcpy(wal_sb.engine_uuid, engine_uuid, kUuidBytes);
        std::memcpy(wal_sb.device_uuid, wal_uuid, kUuidBytes);
        std::memcpy(wal_sb.paired_data_uuid, data_uuid, kUuidBytes);
        wal_sb.device_id = device_id;
        wal_sb.device_count = device_count;
        wal_sb.created_at = now;
        wal_sb.crc32c = ComputeCrc(wal_sb);

        // 快照超级块
        SuperBlock snap_sb{};
        snap_sb.magic = kSuperBlockMagic;
        snap_sb.version = kSuperBlockVersion;
        snap_sb.device_type = static_cast<std::uint32_t>(DeviceType::Snapshot);
        std::memcpy(snap_sb.engine_uuid, engine_uuid, kUuidBytes);
        std::memcpy(snap_sb.device_uuid, snap_uuid, kUuidBytes);
        std::memcpy(snap_sb.paired_data_uuid, data_uuid, kUuidBytes);
        snap_sb.device_id = device_id;
        snap_sb.device_count = device_count;
        snap_sb.created_at = now;
        snap_sb.crc32c = ComputeCrc(snap_sb);

        // 写序：WAL → 快照 → 数据（数据设备最后）。数据超级块是 recover 锚点，最后持久化 →
        // 撕裂的跨设备 create 不会留下"可被发现但不完整"的组（数据缺失即视为未创建）。
        if (WriteSuperBlockPair(wal_dev, wal_sb) != err::kSuccess) return err::kIoBase;
        if (WriteSuperBlockPair(snap_dev, snap_sb) != err::kSuccess) return err::kIoBase;
        if (WriteSuperBlockPair(data_dev, data_sb) != err::kSuccess) return err::kIoBase;

        *out = data_sb;
        return err::kSuccess;
    }

    int32_t RecoverDeviceGroup(const DeviceConfig& cfg, std::uint64_t expected_device_id, std::uint64_t expected_device_count, SuperBlock* out) {
        RawDevice data_dev, wal_dev, snap_dev;
        if (data_dev.Open(cfg.data_path) != err::kSuccess) return err::kIoBase;
        if (wal_dev.Open(cfg.wal_path) != err::kSuccess) return err::kIoBase;
        if (snap_dev.Open(cfg.snapshot_path) != err::kSuccess) return err::kIoBase;

        // 设备最小尺寸校验（与 create 对称）：数据 ≥ 8K+1块；WAL/快照 ≥ 8K
        if (data_dev.SizeBytes() < kDataRegionOffset + kValueSize) return err::kEngineInvalidOpts;
        if (wal_dev.SizeBytes() < kDataRegionOffset) return err::kEngineInvalidOpts;
        if (snap_dev.SizeBytes() < kDataRegionOffset) return err::kEngineInvalidOpts;

        SuperBlock data_sb{}, wal_sb{}, snap_sb{};
        int32_t rc;
        if ((rc = ReadSuperBlockWithRepair(data_dev, &data_sb)) != err::kSuccess) return rc;
        if ((rc = ReadSuperBlockWithRepair(wal_dev, &wal_sb)) != err::kSuccess) return rc;
        if ((rc = ReadSuperBlockWithRepair(snap_dev, &snap_sb)) != err::kSuccess) return rc;

        // 设备类型校验
        if (data_sb.device_type != static_cast<std::uint32_t>(DeviceType::Data) ||
            wal_sb.device_type  != static_cast<std::uint32_t>(DeviceType::Wal)  ||
            snap_sb.device_type != static_cast<std::uint32_t>(DeviceType::Snapshot)) {
            CABE_LOG_ERROR("超级块设备类型不匹配");
            return err::kSuperBlockDeviceTypeMismatch;
        }

        // 引擎 UUID 一致
        if (!UuidEqual(data_sb.engine_uuid, wal_sb.engine_uuid) ||
            !UuidEqual(data_sb.engine_uuid, snap_sb.engine_uuid)) {
            return err::kSuperBlockEngineUuidMismatch;
        }

        // 双向配对：数据.paired_wal == WAL.device 且 WAL.paired_data == 数据.device（快照同理）
        if (!UuidEqual(data_sb.paired_wal_uuid, wal_sb.device_uuid) ||
            !UuidEqual(wal_sb.paired_data_uuid, data_sb.device_uuid) ||
            !UuidEqual(data_sb.paired_snapshot_uuid, snap_sb.device_uuid) ||
            !UuidEqual(snap_sb.paired_data_uuid, data_sb.device_uuid)) {
            return err::kSuperBlockPairMismatch;
        }

        // 设备编号与传入顺序匹配（防盘符漂移）——三设备对称校验
        if (data_sb.device_id != expected_device_id ||
            wal_sb.device_id  != expected_device_id ||
            snap_sb.device_id != expected_device_id) {
            return err::kSuperBlockDeviceIdMismatch;
        }

        // 设备组总数 N 与 create 时一致(防用更短/更长的设备列表 recover:device_id 逐槽仍匹配"成功",
        // 但 RouteKey 的 hash%N 会静默改路由 → 老数据错位不可达。三设备对称校验)。
        if (data_sb.device_count != expected_device_count ||
            wal_sb.device_count  != expected_device_count ||
            snap_sb.device_count != expected_device_count) {
            CABE_LOG_ERROR("设备组总数不符: data_sb=%llu, wal_sb=%llu, snap_sb=%llu, 期望 N=%llu",
                           static_cast<unsigned long long>(data_sb.device_count),
                           static_cast<unsigned long long>(wal_sb.device_count),
                           static_cast<unsigned long long>(snap_sb.device_count),
                           static_cast<unsigned long long>(expected_device_count));
            return err::kSuperBlockDeviceCountMismatch;
        }

        // 持久 block_count 与当前数据设备实际大小核对（防 create 后设备被缩容导致过量供给）。
        const std::uint64_t actual_blocks = (data_dev.SizeBytes() - kDataRegionOffset) / kValueSize;
        if (data_sb.block_count > actual_blocks) {
            CABE_LOG_ERROR("数据设备已缩容: 超级块 block_count=%llu > 实际可容纳 %llu",
                           static_cast<unsigned long long>(data_sb.block_count),
                           static_cast<unsigned long long>(actual_blocks));
            return err::kSuperBlockSizeMismatch;
        }
        // 扩容（actual >）则保留持久 block_count、不自动扩容（扩容需专门的 resize 操作）。

        *out = data_sb;
        return err::kSuccess;
    }

} // namespace cabe
