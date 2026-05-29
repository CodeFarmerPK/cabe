#include "io/uring/io_uring_backend.h"
#include "common/logger.h"

#include <cerrno>
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
        // 数据区从 kDataRegionOffset 起（头部 8K 为双份超级块，P5）
        if (dev_bytes <= kDataRegionOffset) {
            CABE_LOG_ERROR("设备太小: %llu 字节",
                           static_cast<unsigned long long>(dev_bytes));
            ::close(fd_);
            fd_ = -1;
            return err::kEngineInvalidOpts;
        }
        block_count_ = (dev_bytes - kDataRegionOffset) / kValueSize;
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

    namespace {
        // 提交单个 SQE 并等待其完成。健壮处理：submit 在 EINTR/EAGAIN 重试并要求恰好提交 1 个；
        // wait_cqe 在 EINTR 重试（否则在飞 op 会错位收割后续 CQE → 静默错块）；用 user_data 校验
        // 收割到的 CQE 确属本次 block_idx；无论成败恒 cqe_seen 一次，保持 CQ 环一致。
        int32_t SubmitAndWait(struct io_uring* ring, std::uint64_t expect, const char* op) {
            int ret;
            do { ret = io_uring_submit(ring); } while (ret == -EINTR || ret == -EAGAIN);
            if (ret != 1) {
                CABE_LOG_ERROR("io_uring_submit 异常: ret=%d", ret);
                return err::kIoBase;
            }
            struct io_uring_cqe* cqe = nullptr;
            do { ret = io_uring_wait_cqe(ring, &cqe); } while (ret == -EINTR);
            if (ret < 0) {
                CABE_LOG_ERROR("io_uring_wait_cqe 失败: ret=%d", ret);
                return err::kIoBase;
            }
            const std::uint64_t got = io_uring_cqe_get_data64(cqe);
            const int res = cqe->res;
            io_uring_cqe_seen(ring, cqe);
            if (got != expect) {
                CABE_LOG_ERROR("io_uring CQE 错位: 期望 block_idx=%llu 实际=%llu",
                               static_cast<unsigned long long>(expect),
                               static_cast<unsigned long long>(got));
                return err::kIoBase;
            }
            if (res != static_cast<int>(kValueSize)) {
                CABE_LOG_ERROR("io_uring %s 不完整: block_idx=%llu res=%d",
                               op, static_cast<unsigned long long>(expect), res);
                return err::kIoBase;
            }
            return err::kSuccess;
        }
    } // namespace

    int32_t IoUringIoBackend::Write(std::uint64_t block_idx, const std::byte* buf) {
        if (block_idx >= block_count_) {
            CABE_LOG_ERROR("block_idx 越界: %llu >= block_count_=%llu",
                           static_cast<unsigned long long>(block_idx),
                           static_cast<unsigned long long>(block_count_));
            return err::kIoBase;
        }
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            CABE_LOG_ERROR("io_uring_get_sqe 失败");
            return err::kIoBase;
        }
        const auto offset = static_cast<__u64>(kDataRegionOffset + block_idx * kValueSize);
        io_uring_prep_write(sqe, 0, buf, kValueSize, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data64(sqe, block_idx);
        return SubmitAndWait(&ring_, block_idx, "write");
    }

    int32_t IoUringIoBackend::Read(std::uint64_t block_idx, std::byte* buf) {
        if (block_idx >= block_count_) {
            CABE_LOG_ERROR("block_idx 越界: %llu >= block_count_=%llu",
                           static_cast<unsigned long long>(block_idx),
                           static_cast<unsigned long long>(block_count_));
            return err::kIoBase;
        }
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            CABE_LOG_ERROR("io_uring_get_sqe 失败");
            return err::kIoBase;
        }
        const auto offset = static_cast<__u64>(kDataRegionOffset + block_idx * kValueSize);
        io_uring_prep_read(sqe, 0, buf, kValueSize, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data64(sqe, block_idx);
        return SubmitAndWait(&ring_, block_idx, "read");
    }

    bool IoUringIoBackend::is_open() const noexcept {
        return fd_ >= 0;
    }

} // namespace cabe
