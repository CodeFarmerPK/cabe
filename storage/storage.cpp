/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:21
 * Created by: CodeFarmerPK
 */

#include "storage.h"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
namespace {
    // pwrite 的 EINTR-safe + 短写入处理：循环直到全部写完或遇到真实错误。
    // 场景：O_DIRECT 下短写入极少发生，但 ENOSPC 到一半、某些 FS 的边界
    // 行为可能触发 partial write，按 len 目标位置 resume 更健壮。
    // 返回：0..len = 成功写入字节数；-1 = errno 已设置的真实错误。
    ssize_t PWriteAll(int fd, const void* buf, size_t len, off_t offset) {
        const auto* p = static_cast<const char*>(buf);
        size_t done = 0;
        while (done < len) {
            const ssize_t w = ::pwrite(fd, p + done, len - done,
                                       offset + static_cast<off_t>(done));
            if (w < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (w == 0) {
                // pwrite 返回 0 通常意味着文件描述符有问题 / 磁盘用光，
                // 当成失败处理，避免死循环
                errno = EIO;
                return -1;
            }
            done += static_cast<size_t>(w);
        }
        return static_cast<ssize_t>(done);
    }

    // pread 的 EINTR-safe + 短读取处理：同上，循环直到读满或错误 / EOF。
    ssize_t PReadAll(int fd, void* buf, size_t len, off_t offset) {
        auto* p = static_cast<char*>(buf);
        size_t done = 0;
        while (done < len) {
            const ssize_t r = ::pread(fd, p + done, len - done,
                                      offset + static_cast<off_t>(done));
            if (r < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if (r == 0) {
                // EOF（读超出文件末尾）。对 Cabe 的定长块读来说，这是
                // 一个严重错误：block 位置指向了未分配区间
                errno = EIO;
                return -1;
            }
            done += static_cast<size_t>(r);
        }
        return static_cast<ssize_t>(done);
    }
} // namespace

Storage::~Storage() {
    if (fd_ >= 0) {
        Close();
    }
}

int32_t Storage::Open(const std::string& devicePath) {
    if (devicePath.empty()) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    // 先 open 验证可用，成功后才更新 devicePath_，
    // 避免 open 失败时把成员污染成失败的路径名
    const int new_fd = ::open(devicePath.c_str(), O_RDWR | O_DIRECT | O_SYNC);
    if (new_fd < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    // string copy assignment 可能抛 bad_alloc。如果抛了，new_fd 必须关掉
    // 否则 fd 泄漏；同时 Open 必须以错误码而不是异常通知调用方，
    // 维持"int32_t 错误码"的契约。
    try {
        devicePath_ = devicePath;
    } catch (...) {
        ::close(new_fd);
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    fd_ = new_fd;
    return SUCCESS;
}

int32_t Storage::Close() {
    if (fd_ < 0) {
        return SUCCESS;
    }

    // close(2) 语义：无论成功还是失败，fd 都已经被内核释放。
    // 失败后继续持有 fd 号会让后续 close 关到被重新分配的新资源 → UB。
    // 因此无论结果如何都先清零 fd_，再根据 close 返回码决定返回值。
    const int rc = ::close(fd_);
    fd_ = -1;
    devicePath_.clear();
    if (rc < 0) {
        return DEVICE_FAILED_TO_CLOSE_DEVICE;
    }

    return SUCCESS;
}

int32_t Storage::WriteBlock(const BlockId blockId,const DataView data) const {
    if (fd_ < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    if (data.size() != CABE_VALUE_DATA_SIZE) {
        return CABE_INVALID_DATA_SIZE;
    }

    const off_t offset = static_cast<off_t>(blockId) * CABE_VALUE_DATA_SIZE;

    // pwrite: 原子性地定位并写入，线程安全；EINTR 由 PWriteAll 内部重试
    if (const ssize_t written = PWriteAll(fd_, data.data(), CABE_VALUE_DATA_SIZE, offset);
        written < 0 || static_cast<size_t>(written) != CABE_VALUE_DATA_SIZE) {
        return DEVICE_FAILED_TO_WRITE_DATA;
    }

    return SUCCESS;
}

int32_t Storage::ReadBlock(const BlockId blockId, DataBuffer data) const {
    if (fd_ < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    if (data.size() != CABE_VALUE_DATA_SIZE) {
        return CABE_INVALID_DATA_SIZE;
    }

    const off_t offset = static_cast<off_t>(blockId) * CABE_VALUE_DATA_SIZE;

    // pread: 原子性地定位并读取，线程安全；EINTR 由 PReadAll 内部重试
    if (const ssize_t bytesRead = PReadAll(fd_, data.data(), CABE_VALUE_DATA_SIZE, offset);
        bytesRead < 0 || static_cast<size_t>(bytesRead) != CABE_VALUE_DATA_SIZE) {
        return DEVICE_FAILED_TO_READ_DATA;
    }

    return SUCCESS;
}

bool Storage::IsOpen() const {
    return fd_ >= 0;
}