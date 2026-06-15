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

        // P6M1：提交组结果哨兵——正数与错误码空间（≤ 0）零冲突；永不逃出 WriteWal，
        // 故不进 common/error_code.h（D5）。
        inline constexpr int32_t kWalResultPending = 1;

        // P6M1：LIFO 链反转成到达序（= 各 push CAS 在栈修改序中的先后，D11）。
        // 链已被 DrainAll 全摘、离开共享世界（leader 独占），普通指针操作零并发。
        // 模板化以触达 Wal 的私有嵌套节点类型（调用点在成员函数内，访问检查天然通过）。
        template <typename Node>
        Node* ReverseChain(Node* head) noexcept {
            Node* prev = nullptr;
            while (head != nullptr) {
                Node* next = head->next;
                head->next = prev;
                prev = head;
                head = next;
            }
            return prev;
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

    // P6M1：提交组写者节点——每次 WriteWal（同步档）在调用者栈上构造一个，函数返回即消亡。
    // 生命周期三期契约（共享期生产者禁返回 / leader 最后触碰 = result 的 release 写 /
    // 回收期 leader 永不再碰）与 key 有效性三段论见 P6M1 稿 §3.2。
    // result 兼任完成标志（> 0 哨兵 = 未完成；≤ 0 = 最终结果码），但它**不是** wait/notify
    // 挂点——挂点是 commit_.epoch（生命周期与 Wal 等长），封死"notify 打在已亡栈帧"的
    // 未定义行为（D3）。
    struct Wal::WriterNode {
        WalEntry              entry;                     // 原料值拷贝（~40B；key 字节仍在调用者内存）
        WriterNode*           next = nullptr;            // 栈链；可见性由栈顶 CAS 的发布捎带（D4）
        std::atomic<int32_t>  result{kWalResultPending}; // 结果槽兼完成标志
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
        // P6M1（D5）：std::atomic 不可移动——手工 relaxed 搬运。合法前提是契约而非运气：
        // Wal 的移动仅发生在 Open 编排期、提交组静止时（栈空、无人在位、无 waiter），断言钉死。
        assert(o.commit_.stack_head.load(std::memory_order_relaxed) == nullptr
               && !o.commit_.leader_active.load(std::memory_order_relaxed)
               && "Wal 移动必须在提交组静止期（无并发写者）");
        commit_.stack_head.store(
            o.commit_.stack_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
        commit_.epoch.store(
            o.commit_.epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
        commit_.leader_active.store(
            o.commit_.leader_active.load(std::memory_order_relaxed), std::memory_order_relaxed);
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
            // P6M1（D5）：同移动构造——静止期断言 + relaxed 手工搬运。
            assert(o.commit_.stack_head.load(std::memory_order_relaxed) == nullptr
                   && !o.commit_.leader_active.load(std::memory_order_relaxed)
                   && "Wal 移动必须在提交组静止期（无并发写者）");
            commit_.stack_head.store(
                o.commit_.stack_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
            commit_.epoch.store(
                o.commit_.epoch.load(std::memory_order_relaxed), std::memory_order_relaxed);
            commit_.leader_active.store(
                o.commit_.leader_active.load(std::memory_order_relaxed), std::memory_order_relaxed);
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
        // P6M1 入口检查三分（D13）：下面两项是"预检"——纯函数 + 装置态守卫，留在生产者侧、
        // push 之前（非法请求不进栈）；窗口准入（window_bytes_/WindowAt）已迁入 leader 批内。
        if (e.key.size() > kWalKeyMax) return err::kWalKeyTooLong;
        if (cur_buf_ == nullptr) {
            // WAL 未打开，或 recover 模式下 Recover 尚未执行（P5M6 时序防御：Engine 编排保证
            // Open → Recover → 写；本守卫兜住编程错误）。cur_buf_ 装置期定死、运行期只读，
            // 生产者无同步读取安全（与移动语义同款"静止期"论证，D13）。
            CABE_LOG_ERROR("WAL 缓冲未就绪（未打开或恢复未完成），拒绝写入");
            return err::kWalWriteFailed;
        }

        if (sync_level()) {
            // 同步档（级别 1/3），P6M1 提交组：入栈 → 当选裁决 → 在位干活或挂起等待。
            // 多写者并发安全；返回时本帧已持久——批末 fdatasync 程序序先行于 result 回填
            // （release），调用者 acquire 读到结果即接通"落盘先行于返回"（D23）。
            WriterNode node{e};                              // 原料值拷贝（D2）
            if (PushWriter(&node) &&                         // 仅"由空变非空"责任人试当选（D7）
                !commit_.leader_active.exchange(true, std::memory_order_seq_cst)) {
                LeaderLoop();
                // 终值保证（D10）：能当选 ⇒ 前任已回填完毕才让位 ⇒ 此刻自己的结果必为终值。
                // Debug 钉死；Release 下若被未来改动破坏，表现为 WaitResult 挂起（可排查），
                // 而非返回垃圾值。
                assert(node.result.load(std::memory_order_acquire) != kWalResultPending
                       && "leader 出口结果必须已是终值");
            }
            return WaitResult(node);                         // 统一出口（D23）：两角色同道离场
        }

        // 攒批档（级别 2/4）：原路径零改动（单线程契约持续，并发化归 P7，P6-D7/D9）。
        // P5M5 满态惰性重开窗（D12）：回收之后空间恢复，这里自然通过；仍为 0 才对外报满。
        if (window_bytes_ == 0) {
            window_bytes_ = static_cast<std::size_t>(WindowAt(cur_off_));
            if (window_bytes_ == 0) return err::kWalFull;
        }
        // 先清滞留：上次 Flush 失败时窗口内容原样留存，
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

    // ================================================================
    // P6M1 提交组（同步档专用）。协议、内存序与 I1/I2/I3 三证明见
    // doc/P6/P6M1_commit_group_design.md §3/§4；此处注释只标决策号。
    // ================================================================

    bool Wal::PushWriter(WriterNode* n) noexcept {
        WriterNode* old = commit_.stack_head.load(std::memory_order_relaxed);
        do {
            n->next = old;
        } while (!commit_.stack_head.compare_exchange_weak(
                     old, n,
                     std::memory_order_seq_cst,     // 成功：发布节点内容（蕴含 release）+
                                                    //   参与全序 S（Dekker 腿 1 的"写"，D9）
                     std::memory_order_relaxed));   // 失败：只为拿新栈顶重试，不解引用
        return old == nullptr;   // "由空变非空" = 本轮栈生命周期的唯一当选责任人（D7）
    }

    Wal::WriterNode* Wal::DrainAll() noexcept {
        // 破坏性全摘：链整条离栈——消费端无比较故无 ABA；节点归调用者栈所有故无回收问题；
        // acquire 经 RMW 释放序列与全体 push 的发布同步，链上内容完整可见（D4）。
        return commit_.stack_head.exchange(nullptr, std::memory_order_acquire);
    }

    int32_t Wal::WaitResult(WriterNode& node) noexcept {
        // 防丢失唤醒四步（D21）：查结果 → 拍纪元快照 → 再查（纯性能优化，省一次注定空跑的
        // futex 往返；非正确性必需）→ wait(e) 条件入睡（纪元已变则拒睡——封死"唤醒先于
        // 入睡且其后再无唤醒"的卡死时序）。被别批的 notify_all 误醒 = 虚假唤醒，循环消化。
        for (;;) {
            int32_t r = node.result.load(std::memory_order_acquire);
            if (r != kWalResultPending) return r;
            const std::uint64_t e = commit_.epoch.load(std::memory_order_acquire);
            r = node.result.load(std::memory_order_acquire);
            if (r != kWalResultPending) return r;
            commit_.epoch.wait(e, std::memory_order_acquire);
        }
    }

    void Wal::LeaderLoop() {
        // 进入前提：刚以 exchange 取得在位标志（钥匙，I1）——窗口状态/缓冲/盘面自此独占。
        // 跨任期的窗口状态可见性由标志捎带：前任卸任 store 的 release 面发布、本任当选
        // exchange 的 acquire 面承接（D9"交接棒"条款；seq_cst 自含，两操作永不得降级）。
        for (;;) {
            for (;;) {   // ---- S2L 在位循环（D10）----
                WriterNode* batch = DrainAll();
                if (batch != nullptr) {
                    ProcessBatch(ReverseChain(batch));
                }
                // 空批容忍：自己的节点可能已被前任的连任循环处理（终值保证见 WriteWal 注）。
                // 连任复查用 acquire（性能口，允许漏看）——漏看只会提前进卸任序列，
                // 由其 seq_cst 复查兜底（D9 安全网分工）。连任无上限（D12）。
                if (commit_.stack_head.load(std::memory_order_acquire) == nullptr) break;
            }
            // ---- S2X 卸任序列（D8）：让位 → 复查 → 条件再任职；次序不可交换 ----
            // ②①交换的反例：先复查（空）后让位的缝隙里，新写者完成"push + 当选失败"全套
            // 而无人知晓 → 永久遗弃。正序下两操作同落一个原子，必有先后，两分支皆有人兜底。
            commit_.leader_active.store(false, std::memory_order_seq_cst);           // ①
            if (commit_.stack_head.load(std::memory_order_seq_cst) == nullptr) {     // ②
                return;   // 无活，离场
            }
            if (commit_.leader_active.exchange(true, std::memory_order_seq_cst)) {   // ③
                return;   // 他人已接班：其 S2L 首动作 = DrainAll 全摘，现存节点必被覆盖，安心离场
            }
            // ③ 抢回成功 → 连任，回 S2L（无钥匙不得取批——直接 DrainAll 即双写者，违 I1）
        }
    }

    void Wal::ProcessBatch(WriterNode* batch) {
        // 批处理（仅在位者执行）。控制流 = 逐帧[统一准入 → Append] → 末段写出 → 条件
        // fdatasync → 单一失败出口 → 统一三步收尾。结果映射矩阵与状态语义见设计稿 §8。
        bool        wrote   = false;            // 本批是否发生过段写出（条件 fsync，D19）
        bool        dirty   = false;            // 缓冲中是否有本批新增、尚未写出的帧
        int32_t     fail_rc = err::kSuccess;    // 首个失败根因（kWalFull / kWalWriteFailed）
        WriterNode* first_rejected = nullptr;   // 首个未被处理的节点（后缀起点）

        for (WriterNode* n = batch; n != nullptr; n = n->next) {
            // ---- 统一准入（一帧空间；两条进墙路径必须收在同一处，设计 §5.2）----
            if (window_bytes_ == 0) {
                // 路径甲：满态惰性重开（P5M5-D12 语义原样，执行位置迁入 leader，D13）。
                window_bytes_ = static_cast<std::size_t>(WindowAt(cur_off_));
                if (window_bytes_ == 0) { fail_rc = err::kWalFull; first_rejected = n; break; }
            }
            if (static_cast<std::size_t>(n_frames_) * kWalFrameSize >= window_bytes_) {
                // 路径乙：窗口攒满 → 中段写出（无 fsync；必为整块，搬运量恒 0，D14）。
                int32_t rc = WriteOutWindow();
                if (rc != err::kSuccess) {
                    // 写失败：状态未动、帧滞留缓冲（下一批同字节隐式重试，D25）；
                    // 立即停止推进，不带伤继续（D16）。
                    fail_rc = rc; first_rejected = n; break;
                }
                wrote = true;
                dirty = false;
                if (window_bytes_ == 0) { fail_rc = err::kWalFull; first_rejected = n; break; }
            }
            int32_t rc = Append(n->entry);       // seq 单点分配在此（D13：代码不动，资格改由 I1 保证）
            if (rc != err::kSuccess) { fail_rc = rc; first_rejected = n; break; }
            dirty = true;
        }
        // 撞墙时 dirty 必为 false（结构性质 D19：撞墙仅现于开窗关口，每个关口前缓冲刚被
        // 清空或本批尚未产出帧）——故 kWalFull 分支天然没有"末段补写"。

        // ---- 收尾：末段写出 + 条件 fsync → 定前缀/后缀结果 ----
        int32_t prefix_rc;
        if (fail_rc != err::kSuccess && fail_rc != err::kWalFull) {
            prefix_rc = fail_rc;                 // 写失败：根因码给全员、不再碰盘（D24）
        } else {
            int32_t rc = err::kSuccess;
            if (dirty) {
                rc = WriteOutWindow();           // 末段写出（变体 Y 搬运仅可能在此发生，D14）
                if (rc == err::kSuccess) wrote = true;
            }
            if (rc == err::kSuccess && wrote) {
                // 批末唯一 fdatasync——覆盖本批全部段写（O_DIRECT ≠ 持久；块设备 Flush
                // 命令覆盖此前所有已完成写，D16）。零写出批（批首撞墙）无需 fsync。
                if (dev_.Sync() != err::kSuccess) {
                    CABE_LOG_ERROR("WAL 批末 fdatasync 失败（整批改判）");
                    rc = err::kWalWriteFailed;   // 状态保持推进、不回滚——字节已在设备上，
                                                 // 后续批的成功 fsync 顺带确认（幽灵写口径，D25）
                }
            }
            prefix_rc = (rc == err::kSuccess) ? err::kSuccess : err::kWalWriteFailed;
        }
        // 后缀：撞墙批维持 kWalFull 不被前缀的写失败株连（该码对其准确且保留"重试即可"，
        // D19）；写失败批与前缀同码（根因描述失败本身，不描述节点个体处境，D24）。
        const int32_t suffix_rc = (fail_rc == err::kWalFull) ? err::kWalFull : prefix_rc;

        // ---- 统一三步收尾（D22；次序即正确性）：回填(release) → 纪元递增(release) → 广播 ----
        // ①先于②：follower 凭新纪元信任结果为终值的 release 链；②先于③："先改值后通知"
        // 铁律。最后触碰条款（D3）：store 即 leader 对该节点的最后访问——next 必须先取，
        // 因为主人可经 WaitResult 第三步轮询到结果、在纪元递增前就离场，节点随之消亡。
        bool in_suffix = false;
        for (WriterNode* m = batch; m != nullptr; ) {
            if (m == first_rejected) in_suffix = true;
            WriterNode* next = m->next;
            m->result.store(in_suffix ? suffix_rc : prefix_rc, std::memory_order_release);
            m = next;
        }
        commit_.epoch.fetch_add(1, std::memory_order_release);
        commit_.epoch.notify_all();   // notify_one 需链式接力被否决；惊群账见设计稿 §9.2（D22）
    }

    int32_t Wal::Append(const WalEntry& e) {
        if (cur_buf_ == nullptr) {
            CABE_LOG_ERROR("WAL 缓冲未就绪，拒绝 Append");
            return err::kWalWriteFailed;
        }
        // P6M1（D15）：纯"编码 + 拷贝 + 计数"。同步档块推进分支已删——窗口准入统一由
        // 调用方（攒批档 WriteWal / 提交组 ProcessBatch）在 Append 之前完成；攒批档
        // 从未走过被删分支，删除对级别 2/4 零影响。
        const WalFrame f = EncodeFrame(e, seq_next_++);
        std::memcpy(cur_buf_ + static_cast<std::size_t>(n_frames_) * kWalFrameSize, &f, kWalFrameSize);
        ++n_frames_;
        return err::kSuccess;
    }

    // ================================================================
    // P6M1 窗口写出三件套（D14）：Flush 与提交组批路径共用同一套机械——
    // 两档缓冲表示法归一，差异收窄为 fsync 时机 + 返回时机两个策略（D15）。
    // ================================================================

    int32_t Wal::WriteWindowSpan() {
        // 仅写出窗口已用部分（4K 对齐补零），不动任何状态——失败时帧滞留缓冲原样重试。
        const std::size_t used  = static_cast<std::size_t>(n_frames_) * kWalFrameSize;
        const std::size_t bytes = util::AlignUp(used, kWalBlockSize);
        assert(bytes <= ring_end_ - cur_off_ && "刷出越过环尾——窗口不跨缝不变量被破坏");
        if (dev_.WriteAt(cur_off_, cur_buf_, bytes) != err::kSuccess) {
            CABE_LOG_ERROR("WAL 窗口写出失败: off=%llu bytes=%zu",
                           static_cast<unsigned long long>(cur_off_), bytes);
            return err::kWalWriteFailed;
        }
        return err::kSuccess;
    }

    void Wal::AdvanceAfterWrite() noexcept {
        // P5M5 变体 Y（D6）："整块推进、半块留窗"——半块帧挪到缓冲头续攒，下次整块同字节重写
        //   （M2 撕裂安全模式）；提前刷出不再留块尾空洞，活区间内帧紧凑连续（不变量③）。
        //   攒满/中段写出时 used 恰为整块（窗口是 4K 倍数）→ partial=0，自动退化为纯推进。
        const std::size_t used    = static_cast<std::size_t>(n_frames_) * kWalFrameSize;
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
    }

    int32_t Wal::WriteOutWindow() {
        // P6M1 批路径段写：写出 + 推进，**无 fsync**——持久化由批末统一 fdatasync 覆盖
        // （D16 覆盖论证：块设备 Flush 命令覆盖此前所有已完成写）。
        // 写失败不动任何状态——帧滞留缓冲，下一批准入时同偏移同字节隐式重试（D25）。
        if (n_frames_ == 0) return err::kSuccess;   // 空窗（批末可空；fsync 条件由调用方管）
        int32_t rc = WriteWindowSpan();
        if (rc != err::kSuccess) return rc;
        AdvanceAfterWrite();
        return err::kSuccess;
    }

    int32_t Wal::Flush() {
        if (cur_buf_ == nullptr) return err::kSuccess;   // 未打开/恢复未完成：无待刷，空操作
        if (sync_level())        return err::kSuccess;   // 同步档：每批已落盘（P6M1 后欠账恒为
                                                         // 零——已分配 seq 的帧皆持久，留窗帧
                                                         // 只是盘上字节的副本，见 P5M3 §6.5 注）
        if (n_frames_ == 0)      return err::kSuccess;   // 攒批档但缓冲空

        // 攒批档语义逐字保持（P5M5）：写出 → fdatasync → **才**推进；任一步失败状态不动、
        // 内容滞留缓冲、下次原样重试（WriteWal 入口的清滞留逻辑接手）。
        // 与批路径"推进先于 fsync"的次序不对称是有意的（设计 §8.2 辩护）：攒批档帧主人
        // 在入缓冲时已被告知成功，Wal 欠其"确认持久才推进"；同步档批路径欠账为零。
        int32_t rc = WriteWindowSpan();
        if (rc != err::kSuccess) return rc;
        if (dev_.Sync() != err::kSuccess) {
            CABE_LOG_ERROR("WAL fdatasync 失败: off=%llu", static_cast<unsigned long long>(cur_off_));
            return err::kWalWriteFailed;
        }
        AdvanceAfterWrite();
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
