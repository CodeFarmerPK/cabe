#include "snapshot/snapshot.h"

#include "common/error_code.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"

#include <cstring>

namespace cabe {

    int32_t Snapshot::Open(const std::string& path, const Options* opts, const SuperBlock& data_sb) {
        if (opts == nullptr) {   // 调用方编程错误（非设备故障），报"非法 Options"
            CABE_LOG_ERROR("Snapshot::Open 收到空 Options 指针");
            return err::kEngineInvalidOpts;
        }
        int32_t rc = dev_.Open(path);
        if (rc != err::kSuccess) return rc;

        opts_ = opts;
        std::memcpy(engine_uuid_, data_sb.engine_uuid, kUuidBytes);

        // A/B 对半切：公式在 SnapshotSlotSize（snapshot_format.h，与测试共用的单一来源）。
        slot_size_ = SnapshotSlotSize(dev_.SizeBytes());
        if (slot_size_ == 0) {
            CABE_LOG_ERROR("快照设备过小: %llu ≤ 头部 8K",
                           static_cast<unsigned long long>(dev_.SizeBytes()));
            dev_.Close();
            return err::kDeviceTooSmall;
        }
        slot_a_off_ = kDataRegionOffset;
        slot_b_off_ = kDataRegionOffset + slot_size_;

        // 部署期快照设备容量校验：单槽须装得下 [槽头 4K + 向上取整(block_count×128, 4K)]。
        // 上界精确（活键数 ≤ block_count）；运行期不再查。
        const std::uint64_t need = kSnapshotSlotHeaderSize +
            util::AlignUp(data_sb.block_count * kSnapshotRecordSize, kSnapshotBlockSize);
        if (slot_size_ < need) {
            CABE_LOG_ERROR("快照设备过小: 单槽 %llu < 需求 %llu（block_count=%llu）",
                           static_cast<unsigned long long>(slot_size_),
                           static_cast<unsigned long long>(need),
                           static_cast<unsigned long long>(data_sb.block_count));
            dev_.Close();
            return err::kDeviceTooSmall;
        }

        // create-init：清空两槽头（写零 + sync），作废前朝残留槽；next_gen 从 1、无活跃槽。
        // recover 模式的"读两槽头重建 active/代际"在 M6。
        std::byte* z = RawDevice::AllocAligned(kSnapshotSlotHeaderSize);
        if (z == nullptr) {
            CABE_LOG_ERROR("快照槽头清零缓冲分配失败");
            dev_.Close();
            return err::kSnapshotWriteFailed;
        }
        std::memset(z, 0, kSnapshotSlotHeaderSize);
        const int32_t wa = dev_.WriteAt(slot_a_off_, z, kSnapshotSlotHeaderSize);
        const int32_t wb = dev_.WriteAt(slot_b_off_, z, kSnapshotSlotHeaderSize);
        RawDevice::FreeAligned(z);
        if (wa != err::kSuccess || wb != err::kSuccess || dev_.Sync() != err::kSuccess) {
            CABE_LOG_ERROR("快照槽头清零写/刷失败");
            dev_.Close();
            return err::kSnapshotWriteFailed;
        }

        next_gen_         = 1;
        active_slot_      = -1;
        last_covered_seq_ = 0;
        last_trigger_seq_ = 0;
        return err::kSuccess;
    }

    int32_t Snapshot::Write(std::uint64_t covered_seq, const SnapshotScanFn& scan) {
        if (!dev_.is_open()) {
            CABE_LOG_ERROR("Snapshot::Write 快照设备未打开");
            return err::kSnapshotWriteFailed;
        }
        // 退避基准（last_trigger_seq_）不在此推进——由 Engine::DoSnapshot 入口经
        // NoteTriggerAttempt 记账，覆盖"还没到这里就失败"的路径（如刷 WAL 失败）。

        const int target = (active_slot_ == 0) ? 1 : 0;   // 写非活跃槽（active=-1/1 → A，active=0 → B）
        const std::uint64_t slot_off = (target == 0) ? slot_a_off_ : slot_b_off_;
        const std::uint64_t data_off = slot_off + kSnapshotSlotHeaderSize;
        const std::uint64_t data_end = slot_off + slot_size_;

        const std::size_t buf_size = util::RoundUpBufferSize(opts_->snapshot_buffer_size, kSnapshotBlockSize);

        std::byte* buf = RawDevice::AllocAligned(buf_size);
        if (buf == nullptr) {
            CABE_LOG_ERROR("快照缓冲分配失败: %zu 字节", buf_size);
            return err::kSnapshotWriteFailed;
        }

        std::size_t   used  = 0;
        std::uint64_t off   = data_off;
        std::uint64_t count = 0;
        std::uint32_t crc   = 0xFFFFFFFFu;   // CRC32C 初值

        // 编码访问器：每条记录编码进缓冲、累积 data_crc；攒满整块写出。可中止（返错即停）。
        MetaIndexVisitor enc = [&](std::string_view key, const ValueMeta& meta) -> int32_t {
            if (key.size() > kSnapshotKeyMax) return err::kSnapshotWriteFailed;  // 理论 ≤84，防御
            const SnapshotRecord rec = EncodeSnapshotRecord(key, meta);
            std::memcpy(buf + used, &rec, kSnapshotRecordSize);
            crc = util::CRC32CStreamUpdate(crc, DataView{buf + used, kSnapshotRecordSize});
            used += kSnapshotRecordSize;
            ++count;
            if (used == buf_size) {
                if (off + used > data_end) return err::kDeviceTooSmall;          // 越界守卫（容量已校验，理论不触发）
                if (dev_.WriteAt(off, buf, used) != err::kSuccess) return err::kSnapshotWriteFailed;
                off += used;
                used = 0;
            }
            return err::kSuccess;
        };

        int32_t rc = scan(enc);   // = index.ForEach(enc)
        if (rc != err::kSuccess) {
            RawDevice::FreeAligned(buf);
            return rc;
        }

        // 末尾残留：补零到 4K 写出（补的零不计入 data_crc）。
        if (used > 0) {
            const std::size_t bytes = util::AlignUp(used, kSnapshotBlockSize);
            std::memset(buf + used, 0, bytes - used);
            if (off + bytes > data_end) { RawDevice::FreeAligned(buf); return err::kDeviceTooSmall; }
            if (dev_.WriteAt(off, buf, bytes) != err::kSuccess) {
                RawDevice::FreeAligned(buf);
                return err::kSnapshotWriteFailed;
            }
        }

        const std::uint32_t data_crc = ~crc;   // CRC32C 收尾

        // 组装槽头（含 data_crc），写到槽首那 4K，最后一次 fdatasync。槽头因含 data_crc 必须最后写。
        SnapshotSlotHeader hdr{};
        hdr.magic       = kSnapshotSlotMagic;
        hdr.version     = kSnapshotSlotVersion;
        hdr.generation  = next_gen_;
        hdr.covered_seq = covered_seq;
        hdr.entry_count = count;
        hdr.data_len    = count * kSnapshotRecordSize;
        hdr.data_crc    = data_crc;
        hdr.created_at  = util::GetWallTimeNs();
        std::memcpy(hdr.engine_uuid, engine_uuid_, kUuidBytes);
        hdr.header_crc32c = ComputeSlotHeaderCrc(hdr);

        // 槽头复用数据缓冲写出：buf ≥ 4K 且 4K 对齐，数据已全部刷出、内容不再需要——省一次分配。
        std::memcpy(buf, &hdr, sizeof hdr);
        const int32_t hrc = dev_.WriteAt(slot_off, buf, kSnapshotSlotHeaderSize);
        RawDevice::FreeAligned(buf);
        if (hrc != err::kSuccess) {
            CABE_LOG_ERROR("快照槽头写失败: off=%llu", static_cast<unsigned long long>(slot_off));
            return err::kSnapshotWriteFailed;
        }

        if (dev_.Sync() != err::kSuccess) {
            CABE_LOG_ERROR("快照 fdatasync 失败");
            return err::kSnapshotWriteFailed;
        }

        // 成功（且仅刷盘成功后）才更新内存缓存。
        // 注意（M4/M6 口径差）：运行期 next_gen_ 仅在写成功后 +1——失败重试写同一非活跃槽、
        // 复用同号，单会话内安全（目标槽不变，不跨槽撞号）。但 M6 recover 续号必须按设计 §7.1
        // "两槽头中【槽头合法】代际的 max + 1"从盘上重建，不得沿用本内存语义——否则
        // "槽头已落盘但 fdatasync 失败 → 重启"的场景可能代际回退/与盘上残留撞号。
        active_slot_      = target;
        last_covered_seq_ = covered_seq;
        next_gen_        += 1;
        return err::kSuccess;
    }

    int32_t Snapshot::Close() {
        return dev_.Close();   // 未打开时 RawDevice::Close 应为安全空操作
    }

} // namespace cabe
