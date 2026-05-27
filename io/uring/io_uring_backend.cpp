#include "io/uring/io_uring_backend.h"
#include "common/logger.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace cabe {

    IoUringIoBackend::~IoUringIoBackend() {
        if (fd_ >= 0) {
            CABE_LOG_WARN("IoUringIoBackend 析构时仍 Open，自动 Close");
            Close();
        }
    }

    IoUringIoBackend::IoUringIoBackend(IoUringIoBackend&& other) noexcept
        : fd_(other.fd_)
        , block_count_(other.block_count_)
        , ring_(other.ring_)
        , ring_initialized_(other.ring_initialized_)
        , files_registered_(other.files_registered_) {
        other.fd_ = -1;
        other.block_count_ = 0;
        other.ring_initialized_ = false;
        other.files_registered_ = false;
    }

    IoUringIoBackend& IoUringIoBackend::operator=(IoUringIoBackend&& other) noexcept {
        if (this != &other) {
            if (files_registered_) io_uring_unregister_files(&ring_);
            if (ring_initialized_) io_uring_queue_exit(&ring_);
            if (fd_ >= 0) ::close(fd_);

            fd_ = other.fd_;
            block_count_ = other.block_count_;
            ring_ = other.ring_;
            ring_initialized_ = other.ring_initialized_;
            files_registered_ = other.files_registered_;

            other.fd_ = -1;
            other.block_count_ = 0;
            other.ring_initialized_ = false;
            other.files_registered_ = false;
        }
        return *this;
    }

    int32_t IoUringIoBackend::Open(const std::string& path) {
        if (fd_ >= 0) return err::kIoBase;

        fd_ = ::open(path.c_str(), O_RDWR | O_DIRECT, 0);
        if (fd_ < 0) {
            CABE_LOG_ERROR("IoUringIoBackend::Open 打开设备失败: path=%s", path.c_str());
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
            CABE_LOG_ERROR("设备太小: %llu 字节",
                           static_cast<unsigned long long>(dev_bytes));
            ::close(fd_);
            fd_ = -1;
            return err::kEngineInvalidOpts;
        }

        int ret = io_uring_queue_init(kQueueDepth, &ring_, 0);
        if (ret < 0) {
            CABE_LOG_ERROR("io_uring_queue_init 失败: ret=%d", ret);
            ::close(fd_);
            fd_ = -1;
            return err::kIoBase;
        }
        ring_initialized_ = true;

        int32_t fds[] = {fd_};
        ret = io_uring_register_files(&ring_, fds, 1);
        if (ret < 0) {
            CABE_LOG_ERROR("io_uring_register_files 失败: ret=%d", ret);
            io_uring_queue_exit(&ring_);
            ring_initialized_ = false;
            ::close(fd_);
            fd_ = -1;
            return err::kIoBase;
        }
        files_registered_ = true;

        return err::kSuccess;
    }

    int32_t IoUringIoBackend::Close() {
        if (fd_ < 0) return err::kSuccess;
        if (files_registered_) {
            io_uring_unregister_files(&ring_);
            files_registered_ = false;
        }
        if (ring_initialized_) {
            io_uring_queue_exit(&ring_);
            ring_initialized_ = false;
        }
        ::close(fd_);
        fd_ = -1;
        block_count_ = 0;
        return err::kSuccess;
    }

    std::uint64_t IoUringIoBackend::BlockCount() const noexcept {
        return block_count_;
    }

    int32_t IoUringIoBackend::Write(std::uint64_t block_idx, const std::byte* buf) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            CABE_LOG_ERROR("io_uring_get_sqe 失败");
            return err::kIoBase;
        }

        const auto offset = static_cast<__u64>(block_idx * kValueSize);
        io_uring_prep_write(sqe, 0, buf, kValueSize, offset);
        sqe->flags |= IOSQE_FIXED_FILE;

        int ret = io_uring_submit(&ring_);
        if (ret < 0) {
            CABE_LOG_ERROR("io_uring_submit 失败: ret=%d", ret);
            return err::kIoBase;
        }

        struct io_uring_cqe* cqe = nullptr;
        ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0) {
            CABE_LOG_ERROR("io_uring_wait_cqe 失败: ret=%d", ret);
            return err::kIoBase;
        }

        int32_t result = err::kSuccess;
        if (cqe->res != static_cast<int>(kValueSize)) {
            CABE_LOG_ERROR("io_uring write 不完整: block_idx=%llu res=%d",
                           static_cast<unsigned long long>(block_idx), cqe->res);
            result = err::kIoBase;
        }

        io_uring_cqe_seen(&ring_, cqe);
        return result;
    }

    int32_t IoUringIoBackend::Read(std::uint64_t block_idx, std::byte* buf) {
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            CABE_LOG_ERROR("io_uring_get_sqe 失败");
            return err::kIoBase;
        }

        const auto offset = static_cast<__u64>(block_idx * kValueSize);
        io_uring_prep_read(sqe, 0, buf, kValueSize, offset);
        sqe->flags |= IOSQE_FIXED_FILE;

        int ret = io_uring_submit(&ring_);
        if (ret < 0) {
            CABE_LOG_ERROR("io_uring_submit 失败: ret=%d", ret);
            return err::kIoBase;
        }

        struct io_uring_cqe* cqe = nullptr;
        ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0) {
            CABE_LOG_ERROR("io_uring_wait_cqe 失败: ret=%d", ret);
            return err::kIoBase;
        }

        int32_t result = err::kSuccess;
        if (cqe->res != static_cast<int>(kValueSize)) {
            CABE_LOG_ERROR("io_uring read 不完整: block_idx=%llu res=%d",
                           static_cast<unsigned long long>(block_idx), cqe->res);
            result = err::kIoBase;
        }

        io_uring_cqe_seen(&ring_, cqe);
        return result;
    }

    bool IoUringIoBackend::is_open() const noexcept {
        return fd_ >= 0;
    }

} // namespace cabe
