#include "io/uring/io_uring_backend.h"
#include "common/logger.h"

#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <limits>
#include <new>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>
#include <vector>

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
        , files_registered_(other.files_registered_)
        , buffers_registered_(other.buffers_registered_)
        , registered_buffer_count_(other.registered_buffer_count_)
        , registered_buffers_(std::move(other.registered_buffers_))
        , opts_(other.opts_) {
        other.fd_ = -1;
        other.block_count_ = 0;
        other.ring_initialized_ = false;
        other.files_registered_ = false;
        other.buffers_registered_ = false;
        other.registered_buffer_count_ = 0;
        other.registered_buffers_.clear();
        other.opts_ = nullptr;
    }

    IoUringIoBackend& IoUringIoBackend::operator=(IoUringIoBackend&& other) noexcept {
        if (this != &other) {
            Close();

            fd_ = other.fd_;
            block_count_ = other.block_count_;
            ring_ = other.ring_;
            ring_initialized_ = other.ring_initialized_;
            files_registered_ = other.files_registered_;
            buffers_registered_ = other.buffers_registered_;
            registered_buffer_count_ = other.registered_buffer_count_;
            registered_buffers_ = std::move(other.registered_buffers_);
            opts_ = other.opts_;

            other.fd_ = -1;
            other.block_count_ = 0;
            other.ring_initialized_ = false;
            other.files_registered_ = false;
            other.buffers_registered_ = false;
            other.registered_buffer_count_ = 0;
            other.registered_buffers_.clear();
            other.opts_ = nullptr;
        }
        return *this;
    }

    int32_t IoUringIoBackend::Open(const std::string& path, const Options* opts) {
        if (fd_ >= 0) return err::kIoBase;
        opts_ = opts;

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
        if (buffers_registered_) {
            io_uring_unregister_buffers(&ring_);
            buffers_registered_ = false;
            registered_buffer_count_ = 0;
            registered_buffers_.clear();
        }
        if (files_registered_) {
            io_uring_unregister_files(&ring_);
            files_registered_ = false;
        }
        if (ring_initialized_) {
            io_uring_queue_exit(&ring_);
            ring_initialized_ = false;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        block_count_ = 0;
        return err::kSuccess;
    }

    std::uint64_t IoUringIoBackend::BlockCount() const noexcept {
        return block_count_;
    }

    int32_t IoUringIoBackend::RegisterWriteBuffers(std::span<const ValueBufferSlotView> buffers) {
        if (buffers.empty()) return err::kSuccess;
        if (!ring_initialized_ || buffers_registered_) return err::kIoBase;
        if (buffers.size() > std::numeric_limits<unsigned>::max()) return err::kIoBase;

        std::vector<iovec> iovecs;
        std::vector<RegisteredBufferRecord> records;
        std::vector<unsigned char> seen;
        try {
            iovecs.resize(buffers.size());
            records.resize(buffers.size());
            seen.resize(buffers.size(), 0);
        } catch (const std::bad_alloc&) {
            return err::kEnginePoolExhausted;
        }

        for (const auto& view : buffers) {
            if (view.data == nullptr || view.size != kValueSize ||
                view.slot_index >= buffers.size() || seen[view.slot_index] != 0) {
                return err::kIoBase;
            }
            seen[view.slot_index] = 1;
            iovecs[view.slot_index].iov_base = const_cast<std::byte*>(view.data);
            iovecs[view.slot_index].iov_len = view.size;
            records[view.slot_index] = RegisteredBufferRecord{view.data, view.size};
        }

        const int ret = io_uring_register_buffers(
            &ring_, iovecs.data(), static_cast<unsigned>(iovecs.size()));
        if (ret < 0) {
            CABE_LOG_ERROR("io_uring_register_buffers 失败: ret=%d", ret);
            return err::kIoBase;
        }

        registered_buffers_ = std::move(records);
        registered_buffer_count_ = static_cast<std::uint32_t>(registered_buffers_.size());
        buffers_registered_ = true;
        return err::kSuccess;
    }

    bool IoUringIoBackend::UseFixedWriteBuffer(const IoWriteBuffer& buffer) const noexcept {
        if (buffer.kind != IoWriteBufferKind::ValueBufferSlot || !buffers_registered_) {
            return false;
        }
        if (buffer.slot_index >= registered_buffer_count_ ||
            buffer.slot_index >= registered_buffers_.size()) {
            return false;
        }
        const auto& record = registered_buffers_[buffer.slot_index];
        return buffer.data != nullptr &&
               buffer.data == record.data &&
               buffer.size == record.size &&
               buffer.size == kValueSize;
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

    int32_t IoUringIoBackend::Write(std::uint64_t block_idx, const IoWriteBuffer& buffer) {
        if (block_idx >= block_count_) {
            CABE_LOG_ERROR("block_idx 越界: %llu >= block_count_=%llu",
                           static_cast<unsigned long long>(block_idx),
                           static_cast<unsigned long long>(block_count_));
            return err::kIoBase;
        }
        if (buffer.data == nullptr || buffer.size != kValueSize) {
            CABE_LOG_ERROR("写入缓冲区非法: data=%p size=%zu", static_cast<const void*>(buffer.data),
                           buffer.size);
            return err::kIoBase;
        }
        struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            CABE_LOG_ERROR("io_uring_get_sqe 失败");
            return err::kIoBase;
        }
        const auto offset = static_cast<__u64>(kDataRegionOffset + block_idx * kValueSize);
        if (UseFixedWriteBuffer(buffer)) {
            io_uring_prep_write_fixed(
                sqe, 0, buffer.data, kValueSize, offset, static_cast<int>(buffer.slot_index));
        } else {
            io_uring_prep_write(sqe, 0, buffer.data, kValueSize, offset);
        }
        sqe->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data64(sqe, block_idx);
        int32_t rc = SubmitAndWait(&ring_, block_idx, "write");
        if (rc != err::kSuccess) return rc;
        // P5M3：value 持久按 WAL 级别——级别 1/2 做 FUA（fdatasync），3/4 异步（不刷）。
        // opts_ == nullptr 按级别 3：不 FUA。P7 异步化后改用每笔 RWF_DSYNC（见 P5M2 §11）。
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
