#include "snapshot/snapshot.h"

#include "common/error_code.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"

#include <algorithm>
#include <cstring>

namespace cabe {

    namespace {
        // 空记录流的 data_crc（写侧 count=0 时 crc 初值直接收尾）——空快照合法性判定用。
        constexpr std::uint32_t kEmptyDataCrc = ~std::uint32_t{0xFFFFFFFFu};   // = 0
    } // namespace

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
        // 上界精确（活键数 ≤ block_count）；运行期不再查。P5M6-D5：create/recover 一视同仁——
        // recover 下它防的是"快照设备被换成更小分区"（配对 UUID 防不了的配置漂移）。
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

        if (opts->create) {
            // create-init：清空两槽头（写零 + sync），作废前朝残留槽；next_gen 从 1、无活跃槽。
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

            next_gen_           = 1;
            active_slot_        = -1;
            last_covered_seq_   = 0;
            last_trigger_seq_   = 0;
            active_entry_count_ = 0;
            return err::kSuccess;
        }

        // ---- P5M6 recover 分支：双槽裁决 + 状态重建（全程零写盘，坏槽留在盘上作诊断证据）----

        // 读两槽头。读 I/O 错 = 证据不可得 → 拒开（≠"内容无效"——那是证据说"此槽无快照"，
        // 见 P5M6-D6 二分；对称超级块 kSuperBlockReadFailed 先例）。
        std::byte* hb = RawDevice::AllocAligned(kSnapshotSlotHeaderSize);
        if (hb == nullptr) {
            CABE_LOG_ERROR("快照槽头读取缓冲分配失败");
            dev_.Close();
            return err::kSnapshotReadFailed;
        }
        SnapshotSlotHeader h[2];
        const std::uint64_t slot_off[2] = {slot_a_off_, slot_b_off_};
        for (int s = 0; s < 2; ++s) {
            if (dev_.ReadAt(slot_off[s], hb, kSnapshotSlotHeaderSize) != err::kSuccess) {
                CABE_LOG_ERROR("快照槽头读失败（证据不可得，拒开）: 槽%c off=%llu",
                               s == 0 ? 'A' : 'B', static_cast<unsigned long long>(slot_off[s]));
                RawDevice::FreeAligned(hb);
                dev_.Close();
                return err::kSnapshotReadFailed;
            }
            std::memcpy(&h[s], hb, sizeof(SnapshotSlotHeader));
        }
        RawDevice::FreeAligned(hb);

        // 单槽合法性四条（P5M6-D6）：槽头自校验 + engine_uuid 比对 + data_len 交叉 + 记录区不越槽界
        //（后两条保证之后一切读盘动作的安全边界——门口验完，里面不再步步设防）。
        const auto slot_ok = [this](const SnapshotSlotHeader& sh) {
            return VerifySlotHeader(sh)
                && std::memcmp(sh.engine_uuid, engine_uuid_, kUuidBytes) == 0
                && sh.data_len == sh.entry_count * kSnapshotRecordSize
                && kSnapshotSlotHeaderSize + sh.data_len <= slot_size_;
        };
        const bool ok[2] = {slot_ok(h[0]), slot_ok(h[1])};

        // 双合法撞代际 = 证据矛盾（写侧构造保证代际不可能撞号：成功才 +1、失败重试同槽同号）。
        if (ok[0] && ok[1] && h[0].generation == h[1].generation) {
            CABE_LOG_ERROR("快照双槽代际相撞（证据矛盾，拒开）: generation=%llu",
                           static_cast<unsigned long long>(h[0].generation));
            dev_.Close();
            return err::kSnapshotCorrupted;
        }

        // 候选 = 合法头中代际最大者；无合法头 = 合法的"从未快照"形态（covered=0，非错误）。
        int cand = -1;
        if (ok[0] && ok[1]) cand = (h[0].generation > h[1].generation) ? 0 : 1;
        else if (ok[0])     cand = 0;
        else if (ok[1])     cand = 1;

        if (cand < 0) {
            next_gen_           = 1;
            active_slot_        = -1;
            last_covered_seq_   = 0;
            last_trigger_seq_   = 0;
            active_entry_count_ = 0;
            CABE_LOG_INFO("快照裁决: 双槽头皆无效——按\"从未快照\"恢复（covered_seq=0，创世重放）");
            return err::kSuccess;
        }

        // data_crc 全量校验（两遍读的第一遍）+ 坏槽回退梯子（P5M6-D7）。
        int chosen = -1;
        rc = VerifySlotData(cand, h[cand]);
        if (rc == err::kSuccess) {
            chosen = cand;                                  // 常态：候选即选中
        } else if (rc == err::kSnapshotReadFailed) {
            dev_.Close();
            return rc;                                      // 读 I/O 错：证据不可得，拒开
        } else {
            // 候选数据损坏（损坏事实记 ERROR——即便随后回退成功也要醒目，7.3 两级并用）。
            CABE_LOG_ERROR("快照槽%c 代际 %llu 数据损坏（data_crc 不符）",
                           cand == 0 ? 'A' : 'B',
                           static_cast<unsigned long long>(h[cand].generation));
            const int other = 1 - cand;
            if (ok[other]) {
                rc = VerifySlotData(other, h[other]);
                if (rc == err::kSuccess) {
                    chosen = other;                         // 回退动作记 WARN（动作本身合法）
                    CABE_LOG_WARN("回退到代际 %llu（槽%c）——撕裂形态下回收未发生、回退完整；"
                                  "腐烂形态由 WAL 重放 seq 连续性校验兜底识破（P5M6-D7）",
                                  static_cast<unsigned long long>(h[other].generation),
                                  other == 0 ? 'A' : 'B');
                } else if (rc == err::kSnapshotReadFailed) {
                    dev_.Close();
                    return rc;
                }
            }
            if (chosen < 0) {
                // 梯子终点：合法头是"快照发生过"的盘上证据，数据全坏时按 covered=0 假装从未快照
                // 是编造历史——拒开不降级（P5M6-D7；考虑过的降级方案及否决理由见设计稿 §10.5）。
                CABE_LOG_ERROR("快照双槽数据皆损坏（合法槽头证明快照存在过），拒开不降级");
                dev_.Close();
                return err::kSnapshotCorrupted;
            }
        }

        // 状态重建（P5M6-D9）。next_gen 取【盘上一切合法头】的 max+1，绝非"选中槽+1"——
        // 回退场景下后者会让新快照永远输给数据已坏的高代际尸体（代际对盘上证据严格单调不复用）。
        std::uint64_t max_gen = 0;
        if (ok[0]) max_gen = std::max(max_gen, h[0].generation);
        if (ok[1]) max_gen = std::max(max_gen, h[1].generation);
        active_slot_        = chosen;
        next_gen_           = max_gen + 1;
        last_covered_seq_   = h[chosen].covered_seq;
        last_trigger_seq_   = h[chosen].covered_seq;        // 退避基准 = 盘上真实痕迹（P5M6-D4）
        active_entry_count_ = h[chosen].entry_count;

        CABE_LOG_INFO("快照裁决: 槽%c 代际 %llu, covered_seq=%llu, entry_count=%llu, next_gen=%llu",
                      chosen == 0 ? 'A' : 'B',
                      static_cast<unsigned long long>(h[chosen].generation),
                      static_cast<unsigned long long>(last_covered_seq_),
                      static_cast<unsigned long long>(active_entry_count_),
                      static_cast<unsigned long long>(next_gen_));
        return err::kSuccess;
    }

    int32_t Snapshot::VerifySlotData(int slot, const SnapshotSlotHeader& h) {
        if (h.data_len == 0) {
            // 空快照（entry_count=0）是合法快照——做快照那刻索引恰好为空；与"无快照"语义有别。
            return h.data_crc == kEmptyDataCrc ? err::kSuccess : err::kSnapshotCorrupted;
        }
        const std::uint64_t data_off = (slot == 0 ? slot_a_off_ : slot_b_off_) + kSnapshotSlotHeaderSize;
        const std::size_t buf_size = util::RoundUpBufferSize(opts_->snapshot_buffer_size, kSnapshotBlockSize);
        std::byte* buf = RawDevice::AllocAligned(buf_size);
        if (buf == nullptr) {
            CABE_LOG_ERROR("快照数据校验缓冲分配失败: %zu 字节", buf_size);
            return err::kSnapshotReadFailed;
        }
        // 流式读恰好 data_len 字节累积 CRC；末块补零不进 CRC（与写侧口径一致）。
        // 读长按 4K 对齐（O_DIRECT），合法性四条已保证对齐总长不越槽界。
        std::uint64_t off       = data_off;
        std::uint64_t remaining = h.data_len;
        std::uint32_t crc       = 0xFFFFFFFFu;
        while (remaining > 0) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(buf_size, util::AlignUp(static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, buf_size)), kSnapshotBlockSize)));
            if (dev_.ReadAt(off, buf, want) != err::kSuccess) {
                CABE_LOG_ERROR("快照数据区读失败（证据不可得，拒开）: off=%llu",
                               static_cast<unsigned long long>(off));
                RawDevice::FreeAligned(buf);
                return err::kSnapshotReadFailed;
            }
            const std::size_t use = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, want));
            crc = util::CRC32CStreamUpdate(crc, DataView{buf, use});
            remaining -= use;
            off       += want;
        }
        RawDevice::FreeAligned(buf);
        return (~crc == h.data_crc) ? err::kSuccess : err::kSnapshotCorrupted;
    }

    int32_t Snapshot::Load(const MetaIndexVisitor& visitor) {
        if (!dev_.is_open()) {
            CABE_LOG_ERROR("Snapshot::Load 快照设备未打开");
            return err::kSnapshotReadFailed;
        }
        if (active_slot_ < 0 || active_entry_count_ == 0) {
            return err::kSuccess;   // 无快照 / 空快照：空跑成功（Engine 编排零分支）
        }

        const std::uint64_t slot_off = (active_slot_ == 0) ? slot_a_off_ : slot_b_off_;
        const std::size_t buf_size = util::RoundUpBufferSize(opts_->snapshot_buffer_size, kSnapshotBlockSize);
        std::byte* buf = RawDevice::AllocAligned(buf_size);
        if (buf == nullptr) {
            CABE_LOG_ERROR("快照加载缓冲分配失败: %zu 字节", buf_size);
            return err::kSnapshotReadFailed;
        }

        // 纯机械投递：流式读 → 逐条解码 → visitor（裁决已在 Open 收口，data_crc 已验过——
        // 内容位级忠实；语义校验收口 Engine 统一校验表，这里只留内存安全守卫）。
        std::uint64_t off            = slot_off + kSnapshotSlotHeaderSize;
        std::uint64_t remaining_recs = active_entry_count_;
        while (remaining_recs > 0) {
            const std::uint64_t remaining_bytes = remaining_recs * kSnapshotRecordSize;
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(
                buf_size, util::AlignUp(static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining_bytes, buf_size)), kSnapshotBlockSize)));
            if (dev_.ReadAt(off, buf, want) != err::kSuccess) {
                CABE_LOG_ERROR("快照记录区读失败: off=%llu", static_cast<unsigned long long>(off));
                RawDevice::FreeAligned(buf);
                return err::kSnapshotReadFailed;
            }
            const std::uint64_t recs_here =
                std::min<std::uint64_t>(remaining_recs, want / kSnapshotRecordSize);
            for (std::uint64_t i = 0; i < recs_here; ++i) {
                SnapshotRecord r{};
                std::memcpy(&r, buf + static_cast<std::size_t>(i) * kSnapshotRecordSize, sizeof r);
                if (r.key_len > kSnapshotKeyMax) {
                    // 内存安全守卫（解码前必查，防越界取 key）；CRC 过了内容却非法 = 证据矛盾。
                    CABE_LOG_ERROR("快照记录 key_len 越界（证据矛盾）: key_len=%u, 记录序=%llu",
                                   r.key_len,
                                   static_cast<unsigned long long>(active_entry_count_ - remaining_recs + i));
                    RawDevice::FreeAligned(buf);
                    return err::kSnapshotCorrupted;
                }
                std::string_view key;
                ValueMeta meta{};
                DecodeSnapshotRecord(r, &key, &meta);
                const int32_t rc = visitor(key, meta);     // 可中止：返错立停上抛
                if (rc != err::kSuccess) {
                    RawDevice::FreeAligned(buf);
                    return rc;
                }
            }
            remaining_recs -= recs_here;
            off            += want;
        }
        RawDevice::FreeAligned(buf);
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
        // 运行期 next_gen_ 仅在写成功后 +1——失败重试写同一非活跃槽、复用同号，单会话内安全
        //（目标槽不变，不跨槽撞号）。跨重启续号由 recover-Open 按"盘上合法头 max+1"重建
        //（P5M6-D9 已实装，见 Open 的 recover 分支），不沿用本内存语义。
        active_slot_        = target;
        last_covered_seq_   = covered_seq;
        active_entry_count_ = count;
        next_gen_          += 1;
        return err::kSuccess;
    }

    int32_t Snapshot::Close() {
        return dev_.Close();   // 未打开时 RawDevice::Close 应为安全空操作
    }

} // namespace cabe
