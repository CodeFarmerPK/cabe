#include "util/raw_device.h"
#include "util/io_retry.h"
#include "common/logger.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <linux/fs.h>    // BLKGETSIZE64
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

namespace cabe {

    RawDevice::~RawDevice() {
        if (fd_ >= 0) Close();
    }

    RawDevice::RawDevice(RawDevice&& other) noexcept
        : fd_(other.fd_)
        , size_bytes_(other.size_bytes_) {
        other.fd_ = -1;
        other.size_bytes_ = 0;
    }

    RawDevice& RawDevice::operator=(RawDevice&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) Close();
            fd_ = other.fd_;
            size_bytes_ = other.size_bytes_;
            other.fd_ = -1;
            other.size_bytes_ = 0;
        }
        return *this;
    }

    int32_t RawDevice::Open(const std::string& path) {
        if (fd_ >= 0) return err::kIoBase;

        fd_ = ::open(path.c_str(), O_RDWR | O_DIRECT | O_CLOEXEC, 0);
        if (fd_ < 0) {
            CABE_LOG_ERROR("RawDevice::Open 失败: path=%s", path.c_str());
            return err::kIoBase;
        }

        std::uint64_t dev_bytes = 0;
        if (::ioctl(fd_, BLKGETSIZE64, &dev_bytes) < 0) {
            CABE_LOG_ERROR("ioctl BLKGETSIZE64 失败: fd=%d", fd_);
            ::close(fd_);
            fd_ = -1;
            return err::kIoBase;
        }
        size_bytes_ = dev_bytes;
        return err::kSuccess;
    }

    int32_t RawDevice::Close() {
        if (fd_ < 0) return err::kSuccess;
        ::close(fd_);
        fd_ = -1;
        size_bytes_ = 0;
        return err::kSuccess;
    }

    int32_t RawDevice::Sync() {
        if (fd_ < 0) return err::kIoBase;
        // 块设备无 inode 元数据需刷，fdatasync 足够且比 fsync 省；把数据刷出设备易失写缓存。
        return ::fdatasync(fd_) < 0 ? err::kIoBase : err::kSuccess;
    }

    std::uint64_t RawDevice::SizeBytes() const noexcept {
        return size_bytes_;
    }

    int32_t RawDevice::ReadAt(std::uint64_t offset, std::byte* buf, std::size_t len) {
        if (fd_ < 0) return err::kIoBase;  // use-after-move / 未打开守卫（release 安全）
        // O_DIRECT 硬约束：offset/len/buf 必须 4K 对齐。Debug 下断言以在调用点暴露误用，NDEBUG 消除。
        assert(offset % kAlignment == 0 && len % kAlignment == 0 &&
               reinterpret_cast<std::uintptr_t>(buf) % kAlignment == 0 &&
               "RawDevice::ReadAt 要求 4K 对齐");
        // EINTR 重试 + 部分读累加（见 util/io_retry.h）
        if (!io_util::ReadExact(fd_, buf, len, offset)) {
            CABE_LOG_ERROR("RawDevice::ReadAt 失败: offset=%llu len=%zu",
                           static_cast<unsigned long long>(offset), len);
            return err::kIoBase;
        }
        return err::kSuccess;
    }

    int32_t RawDevice::WriteAt(std::uint64_t offset, const std::byte* buf, std::size_t len) {
        if (fd_ < 0) return err::kIoBase;  // use-after-move / 未打开守卫（release 安全）
        // O_DIRECT 硬约束：offset/len/buf 必须 4K 对齐。Debug 下断言以在调用点暴露误用，NDEBUG 消除。
        assert(offset % kAlignment == 0 && len % kAlignment == 0 &&
               reinterpret_cast<std::uintptr_t>(buf) % kAlignment == 0 &&
               "RawDevice::WriteAt 要求 4K 对齐");
        // EINTR 重试 + 部分写累加（见 util/io_retry.h）
        if (!io_util::WriteExact(fd_, buf, len, offset)) {
            CABE_LOG_ERROR("RawDevice::WriteAt 失败: offset=%llu len=%zu",
                           static_cast<unsigned long long>(offset), len);
            return err::kIoBase;
        }
        return err::kSuccess;
    }

    bool RawDevice::is_open() const noexcept {
        return fd_ >= 0;
    }

    std::byte* RawDevice::AllocAligned(std::size_t size) {
        void* p = nullptr;
        if (::posix_memalign(&p, kAlignment, size) != 0) return nullptr;
        return static_cast<std::byte*>(p);
    }

    void RawDevice::FreeAligned(std::byte* p) noexcept {
        ::free(p);
    }

} // namespace cabe
