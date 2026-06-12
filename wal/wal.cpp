#include "wal/wal.h"

#include "common/error_code.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <utility>

namespace cabe {

    namespace {
        // 帧 CRC：覆盖 [0, 124)（即整帧去掉末尾 4 字节的 frame_crc32c 本身）。
        std::uint32_t ComputeFrameCrc(const WalFrame& f) {
            const auto* p = reinterpret_cast<const std::byte*>(&f);
            return util::CRC32(DataView{p, kWalFrameSize - sizeof f.frame_crc32c});
        }

        // 偏移所在 4K 块的起点（恢复定锚/容差区计算用）。
        constexpr std::uint64_t BlockFloor(std::uint64_t off) noexcept {
            return (off / kWalBlockSize) * kWalBlockSize;
        }
    } // namespace

    WalFrame EncodeFrame(const WalEntry& e, std::uint64_t seq) {
        WalFrame f{};                       // 零初始化：key 补零、reserved 清零
        f.magic      = kWalMagic;
        f.version    = static_cast<std::uint8_t>(kWalFrameVersion);
        f.flags      = 0;
        f.entry_type = static_cast<std::uint8_t>(e.type);
        f.reserved0  = 0;
        f.seq        = seq;
        f.block      = e.block.raw;
        f.timestamp  = e.timestamp;
        f.value_crc  = e.value_crc;
        f.key_len    = static_cast<std::uint16_t>(e.key.size());
        f.reserved1  = 0;
        if (!e.key.empty()) {
            std::memcpy(f.key, e.key.data(), e.key.size()); // 调用方保证 ≤ kWalKeyMax
        }
        f.frame_crc32c = ComputeFrameCrc(f);
        return f;
    }

    bool VerifyFrame(const WalFrame& f) {
        if (f.magic != kWalMagic) return false;
        if (f.version != kWalFrameVersion) return false;
        if (f.key_len > kWalKeyMax) return false;
        return ComputeFrameCrc(f) == f.frame_crc32c;
    }

    // P5M6：census 五标量（设计稿 §6.2）。seq 域从 1 起、0 作哨兵——max_valid_seq==0 即"全环无帧"。
    struct Wal::RecoverCensus {
        std::uint64_t valid_count   = 0;   // 合法帧总数（诊断 + "无帧但 covered>0" 矛盾裁决）
        std::uint64_t live_count    = 0;   // 活帧总数（合法 ∧ seq > covered）
        std::uint64_t min_live_seq  = 0;   // 最小活帧 seq / 偏移（走读起点 + head 重建）
        std::uint64_t min_live_off  = 0;
        std::uint64_t max_live_seq  = 0;   // 最大活帧 seq / 偏移（对账）
        std::uint64_t max_live_off  = 0;
        std::uint64_t max_valid_seq = 0;   // 全局最大合法帧（无活帧时的续写锚；有活帧时 == max_live）
        std::uint64_t max_valid_off = 0;
    };

    Wal::~Wal() {
        if (cur_buf_ != nullptr) {
            RawDevice::FreeAligned(cur_buf_);
            cur_buf_ = nullptr;
        }
        // dev_ 由 RawDevice 析构自行 Close
    }

    Wal::Wal(Wal&& o) noexcept
        : dev_(std::move(o.dev_))
        , cur_buf_(o.cur_buf_)
        , buf_size_(o.buf_size_)
        , cur_off_(o.cur_off_)
        , n_frames_(o.n_frames_)
        , seq_next_(o.seq_next_)
        , opts_(o.opts_)
        , ring_start_(o.ring_start_)
        , ring_end_(o.ring_end_)
        , head_off_(o.head_off_)
        , window_bytes_(o.window_bytes_) {
        o.cur_buf_ = nullptr;
    }

    Wal& Wal::operator=(Wal&& o) noexcept {
        if (this != &o) {
            if (cur_buf_ != nullptr) RawDevice::FreeAligned(cur_buf_);
            dev_          = std::move(o.dev_);
            cur_buf_      = o.cur_buf_;
            buf_size_     = o.buf_size_;
            cur_off_      = o.cur_off_;
            n_frames_     = o.n_frames_;
            seq_next_     = o.seq_next_;
            opts_         = o.opts_;
            ring_start_   = o.ring_start_;
            ring_end_     = o.ring_end_;
            head_off_     = o.head_off_;
            window_bytes_ = o.window_bytes_;
            o.cur_buf_ = nullptr;
        }
        return *this;
    }

    bool Wal::sync_level() const noexcept {
        return IsWalSyncLevel(opts_->wal_level);
    }

    std::uint64_t Wal::WindowAt(std::uint64_t pos) const noexcept {
        // 三个量都是 4K 倍数（pos 4K 对齐、环容量 4K 倍数、buf_size_ 取整过）→ 结果天然对齐。
        const std::uint64_t ring    = ring_end_ - ring_start_;
        const std::uint64_t to_seam = ring_end_ - pos;                       // 跨缝截短（D3）
        const std::uint64_t live    = (pos + ring - head_off_) % ring;       // head → pos 的活区字节
        const std::uint64_t limit   = ring - kWalBlockSize;                  // 恒留一块（D5）
        const std::uint64_t adm     = live >= limit ? 0 : limit - live;      // 准入余量（D12）
        return std::min({static_cast<std::uint64_t>(buf_size_), to_seam, adm});
    }

    int32_t Wal::Open(const std::string& wal_path, const Options* opts) {
        if (opts == nullptr) {   // 调用方编程错误（非设备故障）：Wal 必须有 Options 现读 wal_level
            CABE_LOG_ERROR("Wal::Open 收到空 Options 指针");
            return err::kEngineInvalidOpts;
        }
        int32_t rc = dev_.Open(wal_path);
        if (rc != err::kSuccess) return rc;

        opts_ = opts;
        // 缓冲大小 = wal_buffer_size 钳到 ≥4K 并向上取整 4K（O_DIRECT + 整块整帧；与快照共用规整函数）。
        buf_size_ = util::RoundUpBufferSize(opts->wal_buffer_size, kWalBlockSize);

        // P5M5：环几何（单一来源 WalRingSize，D1）。
        const std::uint64_t ring_size = WalRingSize(dev_.SizeBytes());
        ring_start_ = kDataRegionOffset;
        ring_end_   = ring_start_ + ring_size;

        // 部署期容量校验（M5-D16）——ring_size ≥ max(阈值×2, 缓冲+4K)，运行期不再查。
        //   阈值×2：运转基准（增长触发先于撞墙）；缓冲+4K：救援重试必成的数学底线（D13）。
        // P5M6-D5：create/recover 一视同仁（recover 校验欠账在此公共段自然结清）。
        const std::uint64_t thr = opts->snapshot_threshold_bytes;
        const std::uint64_t need_run =
            thr > (~0ull >> 1) ? ~0ull : thr * 2;                            // 防乘法溢出
        const std::uint64_t need_floor =
            static_cast<std::uint64_t>(buf_size_) + kWalBlockSize;
        const std::uint64_t need = std::max(need_run, need_floor);
        if (ring_size < need) {
            CABE_LOG_ERROR("WAL 设备过小: ring=%llu < 需求 max(阈值×2=%llu, 缓冲+4K=%llu)",
                           static_cast<unsigned long long>(ring_size),
                           static_cast<unsigned long long>(need_run),
                           static_cast<unsigned long long>(need_floor));
            dev_.Close();
            return err::kDeviceTooSmall;
        }

        if (!opts->create) {
            // P5M6 recover：缓冲推迟到 Recover() 分配——cur_buf_ 为空使 WriteWal/Append/Flush
            // 的既有守卫天然拒绝"Open 之后、Recover 之前"的过早写入（时序防御，零新状态位）。
            // 环状态置为已定义的中性值，待 Recover 从盘上事实重建。
            cur_buf_      = nullptr;
            cur_off_      = ring_start_;
            head_off_     = ring_start_;
            n_frames_     = 0;
            seq_next_     = 1;
            window_bytes_ = 0;
            return err::kSuccess;
        }

        cur_buf_ = RawDevice::AllocAligned(buf_size_);
        if (cur_buf_ == nullptr) {
            CABE_LOG_ERROR("WAL 缓冲分配失败: %zu 字节", buf_size_);
            dev_.Close();
            return err::kIoBase;
        }
        std::memset(cur_buf_, 0, buf_size_);

        // create：空环（head == tail == ring_start，恒留一块约定下唯一表示空），seq=1。
        cur_off_   = ring_start_;
        head_off_  = ring_start_;
        n_frames_  = 0;
        seq_next_  = 1;
        window_bytes_ = static_cast<std::size_t>(WindowAt(cur_off_));   // 空环必 >0（容量校验已保证）
        return err::kSuccess;
    }

    int32_t Wal::CensusScan(std::uint64_t covered_seq, RecoverCensus* c) {
        // 线性读全环（buf_size_ 分块、贴缝截短），逐 128 字节槽三分类，只采标量不裁决（§6.2）。
        for (std::uint64_t off = ring_start_; off < ring_end_; ) {
            const std::size_t len = static_cast<std::size_t>(
                std::min<std::uint64_t>(buf_size_, ring_end_ - off));
            if (dev_.ReadAt(off, cur_buf_, len) != err::kSuccess) {
                CABE_LOG_ERROR("恢复普查读失败（证据不可得，拒开）: off=%llu len=%zu",
                               static_cast<unsigned long long>(off), len);
                return err::kWalRecoveryReadFailed;
            }
            for (std::size_t p = 0; p < len; p += kWalFrameSize) {
                WalFrame f;
                std::memcpy(&f, cur_buf_ + p, kWalFrameSize);
                if (!VerifyFrame(f)) continue;                       // 无效槽（补零/撕裂/未写过）
                ++c->valid_count;
                const std::uint64_t fo = off + p;
                if (f.seq > c->max_valid_seq) { c->max_valid_seq = f.seq; c->max_valid_off = fo; }
                if (f.seq > covered_seq) {                           // 活帧
                    ++c->live_count;
                    if (c->live_count == 1 || f.seq < c->min_live_seq) {
                        c->min_live_seq = f.seq;
                        c->min_live_off = fo;
                    }
                    if (f.seq > c->max_live_seq) { c->max_live_seq = f.seq; c->max_live_off = fo; }
                }
                // else：残留帧（已被快照覆盖，无害）
            }
            off += len;
        }
        return err::kSuccess;
    }

    int32_t Wal::Recover(std::uint64_t covered_seq, const WalReplayFn& fn) {
        if (!dev_.is_open() || opts_ == nullptr) {
            CABE_LOG_ERROR("Wal::Recover 设备未打开（须先 Open）");
            return err::kEngineInvalidOpts;
        }
        if (cur_buf_ != nullptr) {
            CABE_LOG_ERROR("Wal::Recover 重复调用或 create 模式误调（缓冲已存在）");
            return err::kEngineInvalidOpts;
        }
        cur_buf_ = RawDevice::AllocAligned(buf_size_);   // 兼作扫描缓冲（§6.1：零新分配点）
        if (cur_buf_ == nullptr) {
            CABE_LOG_ERROR("WAL 恢复缓冲分配失败: %zu 字节", buf_size_);
            return err::kIoBase;
        }

        const std::uint64_t ring        = ring_end_ - ring_start_;
        const std::uint64_t total_slots = ring / kWalFrameSize;

        // ---- 第一遍：census 普查 ----
        RecoverCensus c{};
        int32_t rc = CensusScan(covered_seq, &c);
        if (rc != err::kSuccess) return rc;

        // census 层矛盾裁决：covered>0 而全环无一合法帧——帧从不被清除（回收纯内存、覆盖只产新帧），
        // 快照声称有过历史则帧必有残迹（P7 TRIM 实施时此前提需对账，设计稿 §1.3 前瞻）。
        if (c.valid_count == 0 && covered_seq > 0) {
            CABE_LOG_ERROR("恢复矛盾: 快照 covered_seq=%llu 但全环无任何合法帧",
                           static_cast<unsigned long long>(covered_seq));
            return err::kWalRecoveryCorrupted;
        }

        // ---- 第二遍：走读投递 + 余环清扫（从首活帧起恰好再读全环一圈）----
        std::uint64_t replayed       = 0;
        std::uint64_t t_stop         = covered_seq;   // 重放集末 seq（无活帧时退化为 covered）
        std::uint64_t anchor_off     = 0;             // 锚帧 = 重放集末帧 / 无活帧时全局最大合法帧
        std::uint64_t anchor_seq     = 0;
        bool          has_anchor     = false;
        std::uint64_t stop_off       = 0;             // 走读停止槽（第一个非期望槽）
        std::uint64_t fragments      = 0;             // 容差内碎片帧数
        std::uint64_t frag_min_seq   = 0, frag_max_seq = 0;
        std::uint64_t blocks_crossed = 0;             // 走读独立块计数（收尾自检对账用）
        std::uint64_t prev_block     = ~0ull;

        if (c.live_count > 0) {
            // 门槛（§6.3）：历史开头不缺页——活帧必须恰从 covered+1 接上。
            if (c.min_live_seq != covered_seq + 1) {
                CABE_LOG_ERROR("恢复历史缺页: 期望首活帧 seq=%llu, 盘上最小活帧 seq=%llu(off=%llu)",
                               static_cast<unsigned long long>(covered_seq + 1),
                               static_cast<unsigned long long>(c.min_live_seq),
                               static_cast<unsigned long long>(c.min_live_off));
                return err::kWalRecoveryCorrupted;
            }

            std::uint64_t expected  = covered_seq + 1;
            bool          in_run    = true;
            std::uint64_t pos       = c.min_live_off;
            std::uint64_t chunk_off = 0;
            std::size_t   chunk_len = 0;
            bool          chunk_ok  = false;

            for (std::uint64_t i = 0; i < total_slots; ++i) {
                // 取槽：chunk 缓存覆盖 pos（4K 对齐起读、贴缝截短；模环一圈内每块至多读一次）。
                if (!chunk_ok || pos < chunk_off || pos + kWalFrameSize > chunk_off + chunk_len) {
                    chunk_off = BlockFloor(pos);
                    chunk_len = static_cast<std::size_t>(
                        std::min<std::uint64_t>(buf_size_, ring_end_ - chunk_off));
                    if (dev_.ReadAt(chunk_off, cur_buf_, chunk_len) != err::kSuccess) {
                        CABE_LOG_ERROR("恢复走读读失败（证据不可得，拒开）: off=%llu",
                                       static_cast<unsigned long long>(chunk_off));
                        return err::kWalRecoveryReadFailed;
                    }
                    chunk_ok = true;
                }
                WalFrame f;
                std::memcpy(&f, cur_buf_ + (pos - chunk_off), kWalFrameSize);
                const bool valid = VerifyFrame(f);

                bool as_sweep = !in_run;
                if (in_run) {
                    if (valid && f.seq == expected) {
                        // 投递（边走边投递：污染由 Open 整体失败 + AbortOpen 兜底，§6.3）。
                        WalEntry e{};
                        e.type      = static_cast<WalEntryType>(f.entry_type);   // 原样透传，语义解释在 Engine
                        e.key       = std::string_view(reinterpret_cast<const char*>(f.key), f.key_len);
                        e.block     = BlockId{f.block};
                        e.value_crc = f.value_crc;
                        e.timestamp = f.timestamp;
                        rc = fn(e, f.seq);
                        if (rc != err::kSuccess) return rc;          // 可中止：错误原样上抛
                        ++replayed;
                        const std::uint64_t blk = pos / kWalBlockSize;
                        if (blk != prev_block) { ++blocks_crossed; prev_block = blk; }
                        anchor_off = pos;
                        anchor_seq = f.seq;
                        has_anchor = true;
                        t_stop     = f.seq;
                        ++expected;
                    } else {
                        in_run   = false;                            // 走读停止：单判据不满足
                        stop_off = pos;
                        as_sweep = true;                             // 本槽随即进入清扫判定
                    }
                }
                if (as_sweep && valid && f.seq > covered_seq) {
                    // 集外活帧：碎片双条件容差（§6.4）——物理一窗不跨缝 + 序号一窗帧数。
                    const std::uint64_t stop_block   = BlockFloor(stop_off);
                    const std::uint64_t zone_end     = std::min<std::uint64_t>(
                        stop_block + buf_size_, ring_end_);
                    const std::uint64_t max_frag_seq = t_stop + buf_size_ / kWalFrameSize;
                    const bool phys_ok = pos >= stop_off && pos < zone_end;
                    const bool seq_ok  = f.seq > t_stop && f.seq <= max_frag_seq;
                    if (!phys_ok || !seq_ok) {
                        CABE_LOG_ERROR("恢复发现越容差活帧（证据矛盾，拒开）: seq=%llu off=%llu, "
                                       "T_stop=%llu stop_off=%llu zone_end=%llu",
                                       static_cast<unsigned long long>(f.seq),
                                       static_cast<unsigned long long>(pos),
                                       static_cast<unsigned long long>(t_stop),
                                       static_cast<unsigned long long>(stop_off),
                                       static_cast<unsigned long long>(zone_end));
                        return err::kWalRecoveryCorrupted;
                    }
                    ++fragments;
                    if (fragments == 1 || f.seq < frag_min_seq) frag_min_seq = f.seq;
                    if (f.seq > frag_max_seq) frag_max_seq = f.seq;
                }
                pos += kWalFrameSize;
                if (pos == ring_end_) pos = ring_start_;             // 模环（D2）
            }
        }

        // ---- 定锚（D17）：锚帧 = 重放集末帧；无活帧 = 全局最大合法帧；空环 = create 同款 ----
        if (!has_anchor) {
            if (c.valid_count == 0) {
                // 全新空环（covered==0 已由 census 裁决保证）。
                std::memset(cur_buf_, 0, buf_size_);
                cur_off_      = ring_start_;
                head_off_     = ring_start_;
                n_frames_     = 0;
                seq_next_     = 1;
                window_bytes_ = static_cast<std::size_t>(WindowAt(cur_off_));
                CABE_LOG_INFO("WAL 重放完成: 空环（无任何帧），按全新状态续写");
                return err::kSuccess;
            }
            anchor_off = c.max_valid_off;   // 盘上最后一次写入的真实落点（= covered 帧）
            anchor_seq = c.max_valid_seq;
            has_anchor = true;
        }

        cur_off_ = BlockFloor(anchor_off);
        n_frames_ = static_cast<std::uint32_t>((anchor_off - cur_off_) / kWalFrameSize) + 1;
        if (n_frames_ == kWalFramesPerBlock) {
            // 满块边角：推进到下一块（模环），免重灌。
            const std::uint64_t next0 = cur_off_ + kWalBlockSize;
            cur_off_  = (next0 == ring_end_) ? ring_start_ : next0;
            n_frames_ = 0;
        }
        head_off_ = (c.live_count > 0) ? BlockFloor(c.min_live_off) : cur_off_;
        seq_next_ = anchor_seq + 1;   // 稠密续号（碎片即将抹除，复用号段安全；设计稿 §10.3 纠正存照）

        // ---- 碎片抹除（D13，仅有碎片时；恢复链唯一写动作，有界 ≤ 一窗、幂等）----
        // 次序：容差区余块写零（用全零缓冲）→ 重灌尾块 → 尾块同字节重写（帧 + 清零尾槽）→ 一次 fdatasync。
        std::memset(cur_buf_, 0, buf_size_);
        if (fragments > 0) {
            const std::uint64_t stop_block = BlockFloor(stop_off);
            const std::uint64_t zone_end   = std::min<std::uint64_t>(stop_block + buf_size_, ring_end_);
            const std::uint64_t zero_begin = (n_frames_ > 0) ? cur_off_ + kWalBlockSize : cur_off_;
            for (std::uint64_t off = zero_begin; off < zone_end; ) {
                const std::size_t len = static_cast<std::size_t>(
                    std::min<std::uint64_t>(buf_size_, zone_end - off));
                if (dev_.WriteAt(off, cur_buf_, len) != err::kSuccess) {
                    CABE_LOG_ERROR("碎片抹除写失败: off=%llu len=%zu",
                                   static_cast<unsigned long long>(off), len);
                    return err::kWalWriteFailed;
                }
                off += len;
            }
        }
        if (n_frames_ > 0) {
            // 重灌：尾块帧读回缓冲头（恢复变体 Y 留窗形态；块内帧已被 census/走读逐帧验过）。
            if (dev_.ReadAt(cur_off_, cur_buf_, kWalBlockSize) != err::kSuccess) {
                CABE_LOG_ERROR("尾块重灌读失败: off=%llu", static_cast<unsigned long long>(cur_off_));
                return err::kWalRecoveryReadFailed;
            }
            std::memset(cur_buf_ + static_cast<std::size_t>(n_frames_) * kWalFrameSize, 0,
                        buf_size_ - static_cast<std::size_t>(n_frames_) * kWalFrameSize);
            if (fragments > 0) {
                // 尾块重写：帧同字节 + 尾槽清零（把尾块内碎片一并抹掉）——M2 撕裂安全模式。
                if (dev_.WriteAt(cur_off_, cur_buf_, kWalBlockSize) != err::kSuccess) {
                    CABE_LOG_ERROR("碎片抹除尾块重写失败: off=%llu",
                                   static_cast<unsigned long long>(cur_off_));
                    return err::kWalWriteFailed;
                }
            }
        }
        if (fragments > 0) {
            if (dev_.Sync() != err::kSuccess) {
                CABE_LOG_ERROR("碎片抹除 fdatasync 失败");
                return err::kWalWriteFailed;
            }
            CABE_LOG_WARN("撕裂尾碎片已抹除: %llu 帧（seq %llu..%llu）——级别契约内未承诺的帧"
                          "（P5M6-D13；丢弃即兑现级别 2/4 的崩溃语义）",
                          static_cast<unsigned long long>(fragments),
                          static_cast<unsigned long long>(frag_min_seq),
                          static_cast<unsigned long long>(frag_max_seq));
        }

        // ---- 窗口重开（单一来源；可为 0 = 满环恢复态，惰性开窗 + 撞墙救援接管）----
        window_bytes_ = static_cast<std::size_t>(WindowAt(cur_off_));

        // ---- 收尾自检（运行期，非 assert——防扫描/重建代码自身 bug，Release 不可蒸发；§8.2）----
        const std::uint64_t live_geo = (cur_off_ + ring - head_off_) % ring;
        const std::uint64_t expect_live = (c.live_count > 0)
            ? (blocks_crossed - (n_frames_ > 0 ? 1 : 0)) * kWalBlockSize
            : 0;
        const bool ok =
            cur_off_ % kWalBlockSize == 0 && head_off_ % kWalBlockSize == 0
            && cur_off_ >= ring_start_ && cur_off_ < ring_end_
            && head_off_ >= ring_start_ && head_off_ < ring_end_
            && live_geo == expect_live                       // 几何 live 与走读跨度对账（回顾修正 #6）
            && seq_next_ >= covered_seq + 1                  // 铁律对偶：续号不落回覆盖区
            && n_frames_ < kWalFramesPerBlock
            && window_bytes_ == WindowAt(cur_off_)
            && replayed + fragments == c.live_count;         // 活帧账目：重放 + 碎片 = 全部
        if (!ok) {
            CABE_LOG_FATAL("恢复收尾自检不过（内部不变式被破坏）: head=%llu tail=%llu n=%u "
                           "live_geo=%llu expect=%llu seq_next=%llu covered=%llu "
                           "replayed=%llu fragments=%llu live=%llu",
                           static_cast<unsigned long long>(head_off_),
                           static_cast<unsigned long long>(cur_off_),
                           n_frames_,
                           static_cast<unsigned long long>(live_geo),
                           static_cast<unsigned long long>(expect_live),
                           static_cast<unsigned long long>(seq_next_),
                           static_cast<unsigned long long>(covered_seq),
                           static_cast<unsigned long long>(replayed),
                           static_cast<unsigned long long>(fragments),
                           static_cast<unsigned long long>(c.live_count));
            return err::kWalRecoveryInvariant;
        }

        CABE_LOG_INFO("WAL 重放完成: 活帧 %llu, T_stop=%llu, 碎片抹除 %llu, "
                      "head=%llu tail=%llu seq_next=%llu",
                      static_cast<unsigned long long>(replayed),
                      static_cast<unsigned long long>(t_stop),
                      static_cast<unsigned long long>(fragments),
                      static_cast<unsigned long long>(head_off_),
                      static_cast<unsigned long long>(cur_off_),
                      static_cast<unsigned long long>(seq_next_));
        return err::kSuccess;
    }

    int32_t Wal::WriteWal(const WalEntry& e) {
        if (e.key.size() > kWalKeyMax) return err::kWalKeyTooLong;
        if (cur_buf_ == nullptr) {
            // WAL 未打开，或 recover 模式下 Recover 尚未执行（P5M6 时序防御：Engine 编排保证
            // Open → Recover → 写；本守卫兜住编程错误）——拒绝写入，避免空指针解引用。
            CABE_LOG_ERROR("WAL 缓冲未就绪（未打开或恢复未完成），拒绝写入");
            return err::kWalWriteFailed;
        }
        // P5M5 满态惰性重开窗（D12）：回收之后空间恢复，这里自然通过；仍为 0 才对外报满。
        // 两档共用：同步档写"当前块"同样要求窗口覆盖——否则推进停在保留块上时会破坏恒留一块。
        if (window_bytes_ == 0) {
            window_bytes_ = static_cast<std::size_t>(WindowAt(cur_off_));
            if (window_bytes_ == 0) return err::kWalFull;
        }

        if (sync_level()) {
            // 同步档（级别 1/3）：每帧整块落盘，落盘成功才返回。
            int32_t rc = Append(e);
            if (rc != err::kSuccess) return rc;
            return SyncCurrentBlock();
        }

        // 攒批档（级别 2/4）。先清滞留：上次 Flush 失败时窗口内容原样留存，
        // 不先刷出就 Append 会越窗写（窗口 < 缓冲时还可能越缓冲）。
        if (static_cast<std::size_t>(n_frames_) * kWalFrameSize >= window_bytes_) {
            int32_t rc = Flush();
            if (rc != err::kSuccess) return rc;
            if (window_bytes_ == 0) return err::kWalFull;   // 刷出后开新窗失败 = 环满
        }
        int32_t rc = Append(e);
        if (rc != err::kSuccess) return rc;
        // 攒满有效窗口才刷（窗口贴缝时被截短，1.3/D3）；定时刷出留 P7。
        if (static_cast<std::size_t>(n_frames_) * kWalFrameSize >= window_bytes_) {
            return Flush();
        }
        return err::kSuccess;
    }

    int32_t Wal::Append(const WalEntry& e) {
        if (cur_buf_ == nullptr) {
            CABE_LOG_ERROR("WAL 缓冲未就绪，拒绝 Append");
            return err::kWalWriteFailed;
        }
        if (sync_level() && n_frames_ == kWalFramesPerBlock) {
            // 同步档块满：模环推进到下一块（D2）。先在【新起点】做空间准入（D12）——
            // WindowAt == 0 ⇔ 连一块都进不去 ⇔ 环满。失败时帧未编码、seq 未耗，干净拒绝。
            const std::uint64_t next0 = cur_off_ + kWalBlockSize;
            assert(next0 <= ring_end_ && "块推进越过环尾——几何不变量被破坏");
            const std::uint64_t next = (next0 == ring_end_) ? ring_start_ : next0;
            const std::uint64_t w = WindowAt(next);
            if (w == 0) return err::kWalFull;
            cur_off_      = next;
            window_bytes_ = static_cast<std::size_t>(w);
            n_frames_     = 0;
            std::memset(cur_buf_, 0, kWalBlockSize);
        }
        const WalFrame f = EncodeFrame(e, seq_next_++);
        std::memcpy(cur_buf_ + static_cast<std::size_t>(n_frames_) * kWalFrameSize, &f, kWalFrameSize);
        ++n_frames_;
        return err::kSuccess;
    }

    int32_t Wal::SyncCurrentBlock() {
        // 同步档：整块写当前 4K 块 + fdatasync。块内已写帧每次以相同字节重写，对不完整写入安全。
        if (dev_.WriteAt(cur_off_, cur_buf_, kWalBlockSize) != err::kSuccess) {
            CABE_LOG_ERROR("WAL 写失败: off=%llu", static_cast<unsigned long long>(cur_off_));
            return err::kWalWriteFailed;
        }
        if (dev_.Sync() != err::kSuccess) {
            CABE_LOG_ERROR("WAL fdatasync 失败: off=%llu", static_cast<unsigned long long>(cur_off_));
            return err::kWalWriteFailed;
        }
        return err::kSuccess;
    }

    int32_t Wal::Flush() {
        if (cur_buf_ == nullptr) return err::kSuccess;   // 未打开/恢复未完成：无待刷，空操作
        if (sync_level())        return err::kSuccess;   // 同步档：每帧已落盘
        if (n_frames_ == 0)      return err::kSuccess;   // 攒批档但缓冲空

        // 攒批档：把已用部分按 4K 对齐一次性写出 + fdatasync（持久性与 M3 相同）。
        // 失败不动任何状态——内容滞留缓冲，下次原样重试（WriteWal 入口的清滞留逻辑接手）。
        const std::size_t used  = static_cast<std::size_t>(n_frames_) * kWalFrameSize;
        const std::size_t bytes = util::AlignUp(used, kWalBlockSize);
        assert(bytes <= ring_end_ - cur_off_ && "刷出越过环尾——窗口不跨缝不变量被破坏");
        if (dev_.WriteAt(cur_off_, cur_buf_, bytes) != err::kSuccess) {
            CABE_LOG_ERROR("WAL 攒批刷出失败: off=%llu bytes=%zu",
                           static_cast<unsigned long long>(cur_off_), bytes);
            return err::kWalWriteFailed;
        }
        if (dev_.Sync() != err::kSuccess) {
            CABE_LOG_ERROR("WAL fdatasync 失败: off=%llu", static_cast<unsigned long long>(cur_off_));
            return err::kWalWriteFailed;
        }

        // P5M5 变体 Y（D6）："整块推进、半块留窗"——半块帧挪到缓冲头续攒，下次整块同字节重写
        //   （M2 撕裂安全模式）；提前刷出不再留块尾空洞，活区间内帧紧凑连续（不变量③）。
        //   攒满刷出时 used 恰为整块（窗口是 4K 倍数）→ partial=0，自动退化为纯推进。
        const std::size_t partial = used % kWalBlockSize;   // 留窗字节（< 4K）
        const std::size_t advance = used - partial;         // 整块推进量
        if (advance > 0) {
            const std::uint64_t next0 = cur_off_ + advance;
            cur_off_ = (next0 == ring_end_) ? ring_start_ : next0;   // D2 模环（至多恰到尾）
            if (partial > 0) std::memmove(cur_buf_, cur_buf_ + advance, partial);
        }
        n_frames_ = static_cast<std::uint32_t>(partial / kWalFrameSize);
        std::memset(cur_buf_ + partial, 0, buf_size_ - partial);
        // 重开窗（截短 + 准入；可能为 0 = 满——下次写入走惰性重开/报满，D12）。
        window_bytes_ = static_cast<std::size_t>(WindowAt(cur_off_));
        // 注：连续两次快照之间无新帧时，留窗块会被同字节重写一遍——幂等无害，不加脏标记（原型最简）。
        return err::kSuccess;
    }

    int32_t Wal::ReclaimUpTo(std::uint64_t boundary) {
        // 三条纯几何校验（D10）：① 4K 对齐 ② 在环内 ③ 在 [head, tail] 的模环路径上
        // （一条模环距离式子同时防"head 倒退"与"越过 tail 吞活区"；空回收/全量回收取等放行）。
        // 未打开/环未初始化也按校验失败处理（纯防御：Engine 的快照路径有 is_open 守卫）。
        const std::uint64_t ring = ring_end_ - ring_start_;
        bool ok = cur_buf_ != nullptr && ring != 0
               && boundary % kWalBlockSize == 0
               && boundary >= ring_start_ && boundary < ring_end_;
        if (ok) {
            const std::uint64_t d_boundary = (boundary + ring - head_off_) % ring;
            const std::uint64_t d_tail     = (cur_off_ + ring - head_off_) % ring;
            ok = d_boundary <= d_tail;
        }
        if (!ok) {
            CABE_LOG_FATAL("非法回收边界（内部不变式被破坏，head 不动）: boundary=%llu head=%llu "
                           "tail=%llu ring=[%llu,%llu)",
                           static_cast<unsigned long long>(boundary),
                           static_cast<unsigned long long>(head_off_),
                           static_cast<unsigned long long>(cur_off_),
                           static_cast<unsigned long long>(ring_start_),
                           static_cast<unsigned long long>(ring_end_));
            return err::kWalInvalidReclaim;
        }
        // 回收 = 一行内存赋值（D9）：零盘 I/O、不清零——被回收区旧帧由三分类保证无害（设计稿 §6.3）。
        // 铁律（回收不越最新已落地快照的 covered_seq）由调用时机落实：仅快照 Write 成功后被调。
        const std::uint64_t old_head = head_off_;
        head_off_ = boundary;
        TrimReclaimedRange(old_head, head_off_);
        // 注：不在此重开窗——若当前窗口为 0（满态），下一次写入的惰性重开自然按新 head 重算（D12）。
        return err::kSuccess;
    }

    void Wal::TrimReclaimedRange(std::uint64_t old_head, std::uint64_t new_head) {
        // TODO(P7): 经统一 TRIM 设施对 [old_head, new_head) 模环区间（跨缝拆两段）发建议性
        //   discard（sync=BLKDISCARD / io_uring / SPDK 各后端原语）；失败静默，绝不影响回收结果。
        //   与 Engine::TrimDeviceBlock 同款待遇；统一设施由 P7 设计，value/WAL/快照三层各自调用。
        (void)old_head;
        (void)new_head;
    }

    int32_t Wal::Close() {
        int32_t frc = err::kSuccess;
        if (cur_buf_ != nullptr) {
            frc = Flush();   // 优雅关闭前刷净攒批缓冲（同步档 / 空缓冲为空操作）
            RawDevice::FreeAligned(cur_buf_);
            cur_buf_ = nullptr;
        }
        int32_t crc = dev_.Close();
        return frc != err::kSuccess ? frc : crc;
    }

} // namespace cabe
