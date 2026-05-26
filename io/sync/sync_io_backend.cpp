#include "io/sync/sync_io_backend.h"
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
        , block_count_(other.block_count_) {
        other.fd_ = -1;
        other.block_count_ = 0;
    }

    SyncIoBackend& SyncIoBackend::operator=(SyncIoBackend&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) Close();
            fd_ = other.fd_;
            block_count_ = other.block_count_;
            other.fd_ = -1;
            other.block_count_ = 0;
        }
        return *this;
    }

    int32_t SyncIoBackend::Open(const std::string& path) {
        if (fd_ >= 0) return err::kIoBase;

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
        block_count_ = dev_bytes / kValueSize;
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
        const auto offset = static_cast<off_t>(block_idx * kValueSize);
        ssize_t written = ::pwrite(fd_, buf, kValueSize, offset);
        if (written != static_cast<ssize_t>(kValueSize)) {
            CABE_LOG_ERROR("pwrite 失败: fd=%d block_idx=%llu written=%zd",
                           fd_, static_cast<unsigned long long>(block_idx), written);
            return err::kIoBase;
        }
        return err::kSuccess;
    }

    int32_t SyncIoBackend::Read(std::uint64_t block_idx, std::byte* buf) {
        const auto offset = static_cast<off_t>(block_idx * kValueSize);
        ssize_t nread = ::pread(fd_, buf, kValueSize, offset);
        if (nread != static_cast<ssize_t>(kValueSize)) {
            CABE_LOG_ERROR("pread 失败: fd=%d block_idx=%llu nread=%zd",
                           fd_, static_cast<unsigned long long>(block_idx), nread);
            return err::kIoBase;
        }
        return err::kSuccess;
    }

    bool SyncIoBackend::is_open() const noexcept {
        return fd_ >= 0;
    }

} // namespace cabe
