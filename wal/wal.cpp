#include "wal/wal.h"

#include "common/error_code.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"

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
        , opts_(o.opts_) {
        o.cur_buf_ = nullptr;
    }

    Wal& Wal::operator=(Wal&& o) noexcept {
        if (this != &o) {
            if (cur_buf_ != nullptr) RawDevice::FreeAligned(cur_buf_);
            dev_      = std::move(o.dev_);
            cur_buf_  = o.cur_buf_;
            buf_size_ = o.buf_size_;
            cur_off_  = o.cur_off_;
            n_frames_ = o.n_frames_;
            seq_next_ = o.seq_next_;
            opts_     = o.opts_;
            o.cur_buf_ = nullptr;
        }
        return *this;
    }

    bool Wal::sync_level() const noexcept {
        return IsWalSyncLevel(opts_->wal_level);
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

        cur_buf_ = RawDevice::AllocAligned(buf_size_);
        if (cur_buf_ == nullptr) {
            CABE_LOG_ERROR("WAL 缓冲分配失败: %zu 字节", buf_size_);
            dev_.Close();
            return err::kIoBase;
        }
        std::memset(cur_buf_, 0, buf_size_);

        cur_off_  = kDataRegionOffset;   // create：日志从头部 8K 之后起，空日志
        n_frames_ = 0;
        seq_next_ = 1;
        return err::kSuccess;
    }

    int32_t Wal::WriteWal(const WalEntry& e) {
        if (e.key.size() > kWalKeyMax) return err::kWalKeyTooLong;

        int32_t rc = Append(e);
        if (rc != err::kSuccess) return rc;

        if (sync_level()) {
            // 同步档（级别 1/3）：每帧整块落盘，落盘成功才返回。
            return SyncCurrentBlock();
        }
        // 攒批档（级别 2/4）：只攒；缓冲满了才一次性刷（定时刷出留 P7）。
        if (static_cast<std::size_t>(n_frames_) * kWalFrameSize >= buf_size_) {
            return Flush();
        }
        return err::kSuccess;
    }

    int32_t Wal::Append(const WalEntry& e) {
        if (cur_buf_ == nullptr) {
            // WAL 未打开（如 recover 模式 M2/M3 不开 WAL）——拒绝写入，避免空指针解引用。
            CABE_LOG_ERROR("WAL 未打开，拒绝 Append");
            return err::kWalWriteFailed;
        }
        // 同步档：当前 4K 块满 32 帧 → 推进到下一个 4K 块（同步档只用缓冲的第一个块）。
        if (sync_level() && n_frames_ == kWalFramesPerBlock) {
            cur_off_ += kWalBlockSize;
            n_frames_ = 0;
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

        // 攒批档：把已用的整数个 4K 块一次性写出 + fdatasync。
        const std::size_t used  = static_cast<std::size_t>(n_frames_) * kWalFrameSize;
        const std::size_t bytes = util::AlignUp(used, kWalBlockSize);
        if (dev_.WriteAt(cur_off_, cur_buf_, bytes) != err::kSuccess) {
            CABE_LOG_ERROR("WAL 攒批刷出失败: off=%llu bytes=%zu",
                           static_cast<unsigned long long>(cur_off_), bytes);
            return err::kWalWriteFailed;
        }
        if (dev_.Sync() != err::kSuccess) {
            CABE_LOG_ERROR("WAL fdatasync 失败: off=%llu", static_cast<unsigned long long>(cur_off_));
            return err::kWalWriteFailed;
        }
        cur_off_ += bytes;
        n_frames_ = 0;
        std::memset(cur_buf_, 0, buf_size_);
        return err::kSuccess;
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
