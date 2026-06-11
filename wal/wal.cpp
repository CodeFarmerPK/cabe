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

        // P5M5：部署期容量校验（D16）——ring_size ≥ max(阈值×2, 缓冲+4K)，运行期不再查。
        //   阈值×2：运转基准（增长触发先于撞墙）；缓冲+4K：救援重试必成的数学底线（D13）。
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

    int32_t Wal::WriteWal(const WalEntry& e) {
        if (e.key.size() > kWalKeyMax) return err::kWalKeyTooLong;
        if (cur_buf_ == nullptr) {
            // WAL 未打开（如 recover 模式 M2~M5 不开 WAL）——拒绝写入，避免空指针解引用。
            CABE_LOG_ERROR("WAL 未打开，拒绝写入");
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
            CABE_LOG_ERROR("WAL 未打开，拒绝 Append");
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
        if (cur_buf_ == nullptr) return err::kSuccess;   // 未打开：无待刷，空操作
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
