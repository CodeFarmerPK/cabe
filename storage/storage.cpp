/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:21
 * Created by: CodeFarmerPK
 */

#include "storage.h"

#include "common/logger.h"
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>   // BLKGETSIZE64
#include <sys/ioctl.h>
#include <sys/stat.h>
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
            const ssize_t w = ::pwrite(fd, p + done, len - done, offset + static_cast<off_t>(done));
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
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
            const ssize_t r = ::pread(fd, p + done, len - done, offset + static_cast<off_t>(done));
            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }
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
        // 析构无法把错误码 propagate 给调用方；O_DIRECT + O_SYNC 下数据已落盘，
        // close() 失败极罕见（通常是 EIO / EBADF），但若发生应至少留下诊断日志，
        // 否则故障会被静默吞掉。需要确认关闭成功的调用方应显式调 Close()。
        if (const int32_t rc = Close(); rc != SUCCESS) {
            CABE_LOG_ERROR("Storage::~Storage: Close failed (code=%d)", rc);
        }
    }
}

int32_t Storage::Open(const std::string& devicePath) {
    if (devicePath.empty()) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    // --- S_ISBLK 校验必须前置于 open(O_DIRECT) ---
    //
    // Linux 的 do_dentry_open() 在发现 O_DIRECT 但 f_mapping 的 a_ops
    // 不实现 direct_IO 时,open(2) 阶段直接 return EINVAL。/dev/null、
    // /dev/zero 这类字符设备的 fops 不实现 direct I/O,若先 open 再 fstat,
    // 非 block 设备会在 open 阶段被 EINVAL 拒绝,错误码走 IOError 通道,
    // 调用方永远见不到更准确的 DEVICE_NOT_BLOCK_DEVICE → InvalidArgument。
    //
    // 因此先用 ::stat 判文件类型,再 open。::stat 不带 O_DIRECT,对字符设备、
    // 普通文件、目录都能成功取到 st_mode。
    //
    // TOCTOU:stat 和 open 之间有理论上的路径 swap 窗口,但 device_path 是
    // Cabe 配置层信任的输入,不是 per-request 用户输入,不在威胁模型内。
    struct stat st{};
    if (::stat(devicePath.c_str(), &st) < 0) {
        // 路径不存在(ENOENT) / 权限被拒(EACCES) / 符号链接断链 等
        // 统一当作"打不开设备" → IOError
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    if (!S_ISBLK(st.st_mode)) {
        CABE_LOG_WARN("Storage::Open: not a block device, path=%s", devicePath.c_str());
        return DEVICE_NOT_BLOCK_DEVICE;
    }

    // 类型校验过了,现在用 O_RDWR|O_DIRECT|O_SYNC 正式打开。
    // 对合法 block 设备这一步几乎不会失败(权限已在 stat 阶段间接验过);
    // 若仍失败(EINVAL 来自不支持 O_DIRECT 的块设备,极罕见),按 IOError 返回。
    const int new_fd = ::open(devicePath.c_str(), O_RDWR | O_DIRECT | O_SYNC);
    if (new_fd < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    // --- 取设备字节数,立即向下取整为 block 数 ---
    //
    // BLKGETSIZE64 是标准块设备 ioctl,返回 uint64_t 字节数(非扇区数)。
    // 字节数只在这一处局部使用,立即向下取整为 block 数后丢弃。
    // 上层(Engine / FreeList / WriteBlock 越界校验)统一只用 block 数,
    // 不再需要重复 `/ CABE_VALUE_DATA_SIZE` 的算式。
    //
    // 向下取整丢弃的尾部不足 1 chunk 的字节是不可寻址区(blockId 永远不会
    // 指到那里),无副作用。
    uint64_t devBytes = 0;
    if (::ioctl(new_fd, BLKGETSIZE64, &devBytes) < 0) {
        CABE_LOG_ERROR("Storage::Open: BLKGETSIZE64 failed, errno=%d", errno);
        ::close(new_fd);
        return DEVICE_QUERY_FAILED;
    }
    const uint64_t blockCount = devBytes / CABE_VALUE_DATA_SIZE;
    if (blockCount == 0) {
        CABE_LOG_WARN("Storage::Open: device too small, bytes=%llu < chunk=%zu",
            static_cast<unsigned long long>(devBytes), CABE_VALUE_DATA_SIZE);
        ::close(new_fd);
        return DEVICE_TOO_SMALL;
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
    blockCount_ = blockCount;
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
    blockCount_ = 0;
    if (rc < 0) {
        return DEVICE_FAILED_TO_CLOSE_DEVICE;
    }

    return SUCCESS;
}

int32_t Storage::WriteBlock(const BlockId blockId, const DataView data) const {
    if (fd_ < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    if (data.size() != CABE_VALUE_DATA_SIZE) {
        return CABE_INVALID_DATA_SIZE;
    }

    // 越界保护:正常路径上 FreeList 已按 blockCount_ 配置上限,blockId 不会越界。
    // 这里是最后一道防线,防止 P3+ 异步路径下因 race 或外部污染让越界 blockId
    // 真的写到设备末尾外(裸设备越界写返回 EIO,但提前拦下能给出更清晰的错误码)。
    if (blockId >= blockCount_) {
        return DEVICE_NO_SPACE;
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

    // 越界保护同 WriteBlock。
    if (blockId >= blockCount_) {
        return DEVICE_NO_SPACE;
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

uint64_t Storage::BlockCount() const {
    return blockCount_;
}