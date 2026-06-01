#include "io/sync/sync_io_backend.h"
#include "util/io_retry.h"
#include "common/logger.h"

#include <fcntl.h>
#include <linux/fs.h>    // BLKGETSIZE64
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace cabe {

    SyncIoBackend::~SyncIoBackend() {
        if (fd_ >= 0) {
            CABE_LOG_WARN("SyncIoBackend 析构时仍 Open，自动 Close");
            Close();
        }
    }

    SyncIoBackend::SyncIoBackend(SyncIoBackend&& other) noexcept
        : fd_(other.fd_)
        , block_count_(other.block_count_)
        , opts_(other.opts_) {
        other.fd_ = -1;
        other.block_count_ = 0;
        other.opts_ = nullptr;
    }

    SyncIoBackend& SyncIoBackend::operator=(SyncIoBackend&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) Close();
            fd_ = other.fd_;
            block_count_ = other.block_count_;
            opts_ = other.opts_;
            other.fd_ = -1;
            other.block_count_ = 0;
            other.opts_ = nullptr;
        }
        return *this;
    }

    int32_t SyncIoBackend::Open(const std::string& path, const Options* opts) {
        if (fd_ >= 0) return err::kIoBase;
        opts_ = opts;

        fd_ = ::open(path.c_str(), O_RDWR | O_DIRECT, 0);
        if (fd_ < 0) {
            CABE_LOG_ERROR("SyncIoBackend::Open 失败: path=%s", path.c_str());
            return err::kIoBase;
        }

        std::uint64_t dev_bytes = 0;
        if (::ioctl(fd_, BLKGETSIZE64, &dev_bytes) < 0) {
            CABE_LOG_ERROR("ioctl BLKGETSIZE64 失败: fd=%d", fd_);
            ::close(fd_);
            fd_ = -1;
            return err::kIoBase;
        }
        // 数据区从 kDataRegionOffset 起（头部 8K 为双份超级块，P5）
        if (dev_bytes <= kDataRegionOffset) {
            CABE_LOG_ERROR("设备太小: %llu 字节", static_cast<unsigned long long>(dev_bytes));
            ::close(fd_);
            fd_ = -1;
            return err::kEngineInvalidOpts;
        }
        block_count_ = (dev_bytes - kDataRegionOffset) / kValueSize;
        if (block_count_ == 0) {
            CABE_LOG_ERROR("设备太小: %llu 字节", static_cast<unsigned long long>(dev_bytes));
            ::close(fd_);
            fd_ = -1;
            return err::kEngineInvalidOpts;
        }
        return err::kSuccess;
    }

    int32_t SyncIoBackend::Close() {
        if (fd_ < 0) return err::kSuccess;
        ::close(fd_);
        fd_ = -1;
        block_count_ = 0;
        return err::kSuccess;
    }

    std::uint64_t SyncIoBackend::BlockCount() const noexcept {
        return block_count_;
    }

    int32_t SyncIoBackend::Write(std::uint64_t block_idx, const std::byte* buf) {
        if (block_idx >= block_count_) {
            CABE_LOG_ERROR("block_idx 越界: %llu >= block_count_=%llu",
                           static_cast<unsigned long long>(block_idx),
                           static_cast<unsigned long long>(block_count_));
            return err::kIoBase;
        }
        const std::uint64_t offset = kDataRegionOffset + block_idx * kValueSize;
        // EINTR 重试 + 部分写累加（见 util/io_retry.h；sync 是默认后端，信号下也需健壮）
        if (!io_util::WriteExact(fd_, buf, kValueSize, offset)) {
            CABE_LOG_ERROR("pwrite 失败: fd=%d block_idx=%llu",
                           fd_, static_cast<unsigned long long>(block_idx));
            return err::kIoBase;
        }
        // P5M3：value 持久按 WAL 级别——级别 1/2 做 FUA（fdatasync），3/4 异步（不刷）。
        // opts_ == nullptr（bench/test 单独构造）按级别 3：不 FUA。
        const WalLevel lvl = opts_ ? opts_->wal_level : WalLevel::WalSync;
        if (IsValueFuaLevel(lvl)) {
            if (::fdatasync(fd_) < 0) {
                CABE_LOG_ERROR("fdatasync 失败: fd=%d block_idx=%llu",
                               fd_, static_cast<unsigned long long>(block_idx));
                return err::kIoBase;
            }
        }
        return err::kSuccess;
    }

    int32_t SyncIoBackend::Read(std::uint64_t block_idx, std::byte* buf) {
        if (block_idx >= block_count_) {
            CABE_LOG_ERROR("block_idx 越界: %llu >= block_count_=%llu",
                           static_cast<unsigned long long>(block_idx),
                           static_cast<unsigned long long>(block_count_));
            return err::kIoBase;
        }
        const std::uint64_t offset = kDataRegionOffset + block_idx * kValueSize;
        // EINTR 重试 + 部分读累加（见 util/io_retry.h）
        if (!io_util::ReadExact(fd_, buf, kValueSize, offset)) {
            CABE_LOG_ERROR("pread 失败: fd=%d block_idx=%llu",
                           fd_, static_cast<unsigned long long>(block_idx));
            return err::kIoBase;
        }
        return err::kSuccess;
    }

    bool SyncIoBackend::is_open() const noexcept {
        return fd_ >= 0;
    }

} // namespace cabe
