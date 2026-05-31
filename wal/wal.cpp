#include "wal/wal.h"

#include "common/error_code.h"
#include "common/logger.h"
#include "util/crc32.h"

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
        , cur_off_(o.cur_off_)
        , slot_(o.slot_)
        , seq_next_(o.seq_next_)
        , level_(o.level_) {
        o.cur_buf_ = nullptr;
    }

    Wal& Wal::operator=(Wal&& o) noexcept {
        if (this != &o) {
            if (cur_buf_ != nullptr) RawDevice::FreeAligned(cur_buf_);
            dev_      = std::move(o.dev_);
            cur_buf_  = o.cur_buf_;
            cur_off_  = o.cur_off_;
            slot_     = o.slot_;
            seq_next_ = o.seq_next_;
            level_    = o.level_;
            o.cur_buf_ = nullptr;
        }
        return *this;
    }

    int32_t Wal::Open(const std::string& wal_path, WalLevel level) {
        int32_t rc = dev_.Open(wal_path);
        if (rc != err::kSuccess) return rc;

        cur_buf_ = RawDevice::AllocAligned(kWalBlockSize);
        if (cur_buf_ == nullptr) {
            CABE_LOG_ERROR("WAL 当前块缓冲分配失败: %zu 字节", kWalBlockSize);
            dev_.Close();
            return err::kIoBase;
        }
        std::memset(cur_buf_, 0, kWalBlockSize);

        cur_off_  = kDataRegionOffset;   // create：日志从头部 8K 之后起，空日志
        slot_     = 0;
        seq_next_ = 1;
        level_    = level;
        return err::kSuccess;
    }

    int32_t Wal::WriteWal(const WalEntry& e) {
        if (e.key.size() > kWalKeyMax) return err::kWalKeyTooLong;

        int32_t rc = Append(e);
        if (rc != err::kSuccess) return rc;

        // M2：统一按级别 1（Strict）——Append 后立即整块落盘，落盘成功才返回。
        // M3 起据 level_ 分支：同步级即时 Sync()，异步级仅 Append() 交背景刷出。
        (void)level_;
        return Sync();
    }

    int32_t Wal::Append(const WalEntry& e) {
        if (cur_buf_ == nullptr) {
            // WAL 未打开（如 M2 的 recover 模式不开 WAL）——拒绝写入，避免空指针解引用。
            CABE_LOG_ERROR("WAL 未打开，拒绝 Append");
            return err::kWalWriteFailed;
        }
        if (slot_ == kWalFramesPerBlock) {
            // 当前 4K 块已满，前进到下一块（M2 线性增长，不判满；环形回收见 M4）。
            cur_off_ += kWalBlockSize;
            slot_ = 0;
            std::memset(cur_buf_, 0, kWalBlockSize);
        }
        const WalFrame f = EncodeFrame(e, seq_next_++);
        std::memcpy(cur_buf_ + slot_ * kWalFrameSize, &f, kWalFrameSize);
        ++slot_;
        return err::kSuccess;
    }

    int32_t Wal::Sync() {
        // 整块写当前 4K 块 + fdatasync（级别 1：落盘后才返回）。
        // 块内已写帧每次以完全相同字节重写，torn-write 安全（见 P5M2 §5.4）。
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

    int32_t Wal::Close() {
        if (cur_buf_ != nullptr) {
            RawDevice::FreeAligned(cur_buf_);
            cur_buf_ = nullptr;
        }
        return dev_.Close();
    }

} // namespace cabe
