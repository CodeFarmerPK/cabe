/*
 * Project: Cabe
 * Created Time: 2026-04-27
 * Created by: CodeFarmerPK
 *
 * SyncIoBackend P3 M2 实现 —— sync 后端真实逻辑。
 *
 * 来源迁移:
 *   - 设备路径 / S_ISBLK / O_DIRECT / pread / pwrite / BLKGETSIZE64 等
 *     从 storage/storage.cpp 搬入(逻辑等价,变量更名为 fd_ / blockCount_ 与
 *     SyncIoBackend 对齐)。
 *   - mmap pool / freeStack LIFO / 越界 + double-release 检查
 *     从 buffer/buffer_pool.cpp 搬入(逻辑等价,锁名换为 poolMutex_)。
 *
 * 与原版相比的契约变化(决策出处见 project_roadmap.md Q1–Q7):
 *   Q1  Acquire/Release 改用 BufferHandle RAII,无显式 Release 方法
 *   Q2  AcquireBuffer 不再 memset,buffer 内容未定义;double-release 检测仍保留
 *   Q3  池耗尽:返回 invalid BufferHandle(对应底层 POOL_EXHAUSTED 语义)
 *   Q7  Close 时仍有 outstanding handle:Debug abort / Release warn + force-release;
 *       force-release 后 BufferHandleImpl::~ 仅 dec count,不再触碰 pool
 *
 * 状态机(终态 Close):
 *   - 同实例只允许 Open → Close 一轮。再次 Open 返回 IO_BACKEND_ALREADY_OPEN。
 *   - Close-before-Open 是幂等 no-op,不进入 terminal,仍可 Open。
 *
 * 锁:
 *   - 设备字段(fd_/blockCount_/devicePath_)由调用方(Engine 的 mutex_)串行
 *   - poolMutex_ 保护 poolBase_/freeStack_;AcquireBuffer 与 ReturnBuffer_Internal 共用
 *   - closed_ atomic 提供 BufferHandleImpl::~ 的 fast-path 判断
 */

#include "io/backends/sync_io_backend.h"
#include "io/backends/sync_buffer_handle_impl.h"

#include "common/error_code.h"
#include "common/logger.h"

#include <cerrno>
#include <cstdlib>
#include <utility>

#include <fcntl.h>
#include <linux/fs.h>       // BLKGETSIZE64
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cabe::io {

namespace {

// pwrite 的 EINTR-safe + 短写入处理:循环直到全部写完或遇到真实错误。
// 与原 storage.cpp::PWriteAll 行为完全等价,迁移时未调整。
ssize_t PWriteAll(int fd, const void* buf, std::size_t len, off_t offset) {
    const auto* p = static_cast<const char*>(buf);
    std::size_t done = 0;
    while (done < len) {
        const ssize_t w = ::pwrite(fd, p + done, len - done,
                                   offset + static_cast<off_t>(done));
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) {
            errno = EIO;
            return -1;
        }
        done += static_cast<std::size_t>(w);
    }
    return static_cast<ssize_t>(done);
}

// pread 的 EINTR-safe + 短读取处理:循环直到读满或错误 / EOF。
// 与原 storage.cpp::PReadAll 行为完全等价。
ssize_t PReadAll(int fd, void* buf, std::size_t len, off_t offset) {
    auto* p = static_cast<char*>(buf);
    std::size_t done = 0;
    while (done < len) {
        const ssize_t r = ::pread(fd, p + done, len - done,
                                  offset + static_cast<off_t>(done));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) {
            // EOF。对定长块读来说是严重错误:blockId 指向了未分配区间
            errno = EIO;
            return -1;
        }
        done += static_cast<std::size_t>(r);
    }
    return static_cast<ssize_t>(done);
}

} // namespace

// =====================================================================
// BufferHandleImpl::~BufferHandleImpl
// 必须放此文件 —— 需要 SyncIoBackend 完整类型才能调 ReturnBuffer_Internal。
// =====================================================================
BufferHandleImpl::~BufferHandleImpl() {
    if (owner_ != nullptr) {
        owner_->ReturnBuffer_Internal(*this);
    }
    // owner_ == nullptr 即 invalid handle:啥也没 acquire 过,无需归还。
    // closed_ 检查 + outstanding_count_ 递减统一在 ReturnBuffer_Internal 里
    // 完成,~BufferHandleImpl 只负责"分发到 owner",不重复决策。
}

// =====================================================================
// SyncIoBackend
// =====================================================================

SyncIoBackend::~SyncIoBackend() {
    if (opened_) {
        // 析构无法把错误码 propagate;Close 内部对 fd close / munmap 失败会
        // 自行 log。这里只是 RAII 兜底。
        (void) Close();
    }
}

int32_t SyncIoBackend::Open(const std::string& devicePath,
                            const std::uint32_t bufferPoolCount) {
    // ---- 状态机守卫 ----
    if (closed_.load(std::memory_order_acquire)) {
        // 已经走过一轮 Close → terminal。想再开必须销毁实例。
        return IO_BACKEND_ALREADY_OPEN;
    }
    if (opened_) {
        return IO_BACKEND_ALREADY_OPEN;
    }

    // ---- 参数校验 ----
    if (devicePath.empty()) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    if (bufferPoolCount == 0) {
        return POOL_INVALID_PARAMS;
    }

    // ===== 第 1 阶段:打开裸块设备(等价 storage::Open)=====
    //
    // S_ISBLK 必须前置于 open(O_DIRECT) —— Linux do_dentry_open 在 O_DIRECT
    // 但 a_ops 不实现 direct_IO 时直接 EINVAL,先 stat 再 open 才能给出
    // 准确的 DEVICE_NOT_BLOCK_DEVICE。
    struct stat st{};
    if (::stat(devicePath.c_str(), &st) < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    if (!S_ISBLK(st.st_mode)) {
        CABE_LOG_WARN("SyncIoBackend::Open: not a block device, path=%s",
                      devicePath.c_str());
        return DEVICE_NOT_BLOCK_DEVICE;
    }

    const int new_fd = ::open(devicePath.c_str(), O_RDWR | O_DIRECT | O_SYNC);
    if (new_fd < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    // ---- 设备字节数 → 块数(向下取整)----
    std::uint64_t devBytes = 0;
    if (::ioctl(new_fd, BLKGETSIZE64, &devBytes) < 0) {
        CABE_LOG_ERROR("SyncIoBackend::Open: BLKGETSIZE64 failed, errno=%d", errno);
        ::close(new_fd);
        return DEVICE_QUERY_FAILED;
    }
    const std::uint64_t newBlockCount = devBytes / CABE_VALUE_DATA_SIZE;
    if (newBlockCount == 0) {
        CABE_LOG_WARN("SyncIoBackend::Open: device too small, bytes=%llu",
                      static_cast<unsigned long long>(devBytes));
        ::close(new_fd);
        return DEVICE_TOO_SMALL;
    }

    // ===== 第 2 阶段:mmap pool(等价 buffer_pool::Init)=====
    const std::size_t totalSize =
        CABE_VALUE_DATA_SIZE * static_cast<std::size_t>(bufferPoolCount);
    void* mmap_ptr = ::mmap(nullptr, totalSize,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                            -1, 0);
    if (mmap_ptr == MAP_FAILED) {
        ::close(new_fd);
        return POOL_MMAP_FAILED;
    }

    // ===== 第 3 阶段:本地构造 freeStack 与 devicePath copy(可能 throw)=====
    std::vector<std::uint32_t> newStack;
    std::string                newPath;
    try {
        newStack.reserve(bufferPoolCount);
        // 倒序压入,pop_back 时按地址顺序分配
        for (std::uint32_t i = bufferPoolCount; i > 0; --i) {
            newStack.push_back(i - 1);
        }
        newPath = devicePath;
    } catch (...) {
        ::munmap(mmap_ptr, totalSize);
        ::close(new_fd);
        // 区分:这里是内存分配失败,不是设备打开失败。复用 POOL_MMAP_FAILED
        // 表达"pool 相关分配/初始化失败"。
        return POOL_MMAP_FAILED;
    }

    // ===== 第 4 阶段:全部成功,no-throw 提交 =====
    fd_              = new_fd;
    blockCount_      = newBlockCount;
    devicePath_      = std::move(newPath);
    poolBase_        = static_cast<char*>(mmap_ptr);
    poolBufferCount_ = bufferPoolCount;
    poolTotalSize_   = totalSize;
    freeStack_       = std::move(newStack);
    opened_          = true;
    return SUCCESS;
}

int32_t SyncIoBackend::Close() {
    if (!opened_) {
        // Close-before-Open 或重复 Close:幂等 no-op,不进入 terminal,
        // 也不设 closed_(允许后续 Open)。
        return SUCCESS;
    }

    // ---- Q7:outstanding handle 检查 ----
    const std::uint32_t pending = outstanding_count_.load(std::memory_order_acquire);
    if (pending != 0) {
#ifndef NDEBUG
        CABE_LOG_ERROR("SyncIoBackend::Close: %u outstanding handles, aborting (Debug)",
                       pending);
        std::abort();
#else
        CABE_LOG_ERROR("SyncIoBackend::Close: %u outstanding handles, force-releasing",
                       pending);
        // 继续 Close;后续 ~BufferHandleImpl 进入 ReturnBuffer_Internal 时会查到
        // closed_ = true,只 dec outstanding_count_ 不触碰 pool 状态。
#endif
    }

    // ---- 标记 closed_ ——必须先于 pool munmap ----
    // 已经持有 handle 还未析构的线程,fast-path 会读到 closed_ = true,
    // 跳过 poolMutex_ 加锁;就算 fast-path 没读到,也会在 ReturnBuffer_Internal
    // 内拿锁后再次 check(此时 Close 已 munmap 过,但 dec count 不需要 pool 状态)。
    closed_.store(true, std::memory_order_release);

    // ---- Pool 清理(锁内,与 ReturnBuffer_Internal 互斥)----
    int32_t poolRc = SUCCESS;
    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        if (poolBase_ != nullptr) {
            if (::munmap(poolBase_, poolTotalSize_) != 0) {
                poolRc = POOL_MMAP_FAILED;
                CABE_LOG_ERROR("SyncIoBackend::Close: munmap failed, errno=%d", errno);
            }
            poolBase_        = nullptr;
            poolBufferCount_ = 0;
            poolTotalSize_   = 0;
            freeStack_.clear();
        }
    }

    // ---- 设备清理 ----
    int32_t devRc = SUCCESS;
    if (fd_ >= 0) {
        // close(2):无论成功失败,fd 都已被内核释放,必须先清零 fd_。
        const int rc = ::close(fd_);
        fd_ = -1;
        devicePath_.clear();
        blockCount_ = 0;
        if (rc < 0) {
            devRc = DEVICE_FAILED_TO_CLOSE_DEVICE;
        }
    }

    opened_ = false;

    // 优先返回设备 close 错误(更严重),其次返回 pool 错误。
    if (devRc != SUCCESS) return devRc;
    return poolRc;
}

bool SyncIoBackend::IsOpen() const noexcept {
    return opened_ && !closed_.load(std::memory_order_acquire);
}

std::uint64_t SyncIoBackend::BlockCount() const noexcept {
    return opened_ ? blockCount_ : 0;
}

BufferHandle SyncIoBackend::AcquireBuffer() {
    if (!opened_ || closed_.load(std::memory_order_acquire)) {
        return BufferHandle{};       // invalid → 调用方靠 .valid() 检查
    }

    // ---- 从空闲栈取 slot ----
    std::uint32_t slot_index;
    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        if (freeStack_.empty()) {
            // Q3:池耗尽立刻失败,不阻塞。
            return BufferHandle{};
        }
        slot_index = freeStack_.back();
        freeStack_.pop_back();
    }

    char* const ptr =
        poolBase_ + static_cast<std::size_t>(slot_index) * CABE_VALUE_DATA_SIZE;

    // Q2:**不 memset!** buffer 内容未定义,调用方负责覆盖。

    // ---- 构造 Impl ——可能 throw bad_alloc,需要回滚 slot ----
    std::unique_ptr<BufferHandleImpl> impl;
    try {
        impl = std::make_unique<BufferHandleImpl>();
    } catch (...) {
        std::lock_guard<std::mutex> lock(poolMutex_);
        freeStack_.push_back(slot_index);   // 回滚:capacity 已 reserve,不会再 throw
        return BufferHandle{};
    }

    impl->ptr_        = ptr;
    impl->size_       = CABE_VALUE_DATA_SIZE;
    impl->slot_index_ = slot_index;
    impl->owner_      = this;

    // 计数 ++ 必须放在 BufferHandle 构造之前,以防异常导致计数与 slot 不平衡
    // (BufferHandle 接管 unique_ptr 的构造是 noexcept,这步之后必无 throw)。
    outstanding_count_.fetch_add(1, std::memory_order_acq_rel);
    return BufferHandle{std::move(impl)};
}

int32_t SyncIoBackend::WriteBlock(const BlockId blockId,
                                  const BufferHandle& handle) {
    if (!opened_ || closed_.load(std::memory_order_acquire)) {
        return IO_BACKEND_NOT_OPEN;
    }
    if (!handle.valid()) {
        return IO_BACKEND_INVALID_HANDLE;
    }
    if (handle.size() != CABE_VALUE_DATA_SIZE) {
        return CABE_INVALID_DATA_SIZE;
    }

    // 验证 handle 来自本实例的 pool(防跨实例误用)。
    // 不持 poolMutex_:poolBase_ / poolTotalSize_ 在 opened_ 之后到 Close 之前
    // 都不变,且本路径的 opened_ 检查已通过,这里读是安全的。
    const char* const dataPtr = handle.data();
    if (dataPtr < poolBase_ || dataPtr >= poolBase_ + poolTotalSize_) {
        return IO_BACKEND_INVALID_HANDLE;
    }

    // 越界保护:正常路径上 FreeList(在 Engine 层)已按 blockCount_ 上限拦住,
    // 此处是最后防线。
    if (blockId >= blockCount_) {
        return DEVICE_NO_SPACE;
    }

    const off_t offset = static_cast<off_t>(blockId) * CABE_VALUE_DATA_SIZE;
    const ssize_t written = PWriteAll(fd_, dataPtr, CABE_VALUE_DATA_SIZE, offset);
    if (written < 0 || static_cast<std::size_t>(written) != CABE_VALUE_DATA_SIZE) {
        return DEVICE_FAILED_TO_WRITE_DATA;
    }
    return SUCCESS;
}

int32_t SyncIoBackend::ReadBlock(const BlockId blockId, BufferHandle& handle) {
    if (!opened_ || closed_.load(std::memory_order_acquire)) {
        return IO_BACKEND_NOT_OPEN;
    }
    if (!handle.valid()) {
        return IO_BACKEND_INVALID_HANDLE;
    }
    if (handle.size() != CABE_VALUE_DATA_SIZE) {
        return CABE_INVALID_DATA_SIZE;
    }

    char* const dataPtr = handle.data();
    if (dataPtr < poolBase_ || dataPtr >= poolBase_ + poolTotalSize_) {
        return IO_BACKEND_INVALID_HANDLE;
    }

    if (blockId >= blockCount_) {
        return DEVICE_NO_SPACE;
    }

    const off_t offset = static_cast<off_t>(blockId) * CABE_VALUE_DATA_SIZE;
    const ssize_t bytesRead = PReadAll(fd_, dataPtr, CABE_VALUE_DATA_SIZE, offset);
    if (bytesRead < 0 || static_cast<std::size_t>(bytesRead) != CABE_VALUE_DATA_SIZE) {
        return DEVICE_FAILED_TO_READ_DATA;
    }
    return SUCCESS;
}

bool SyncIoBackend::is_closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

void SyncIoBackend::ReturnBuffer_Internal(BufferHandleImpl& impl) noexcept {
    // outstanding_count_ 永远 dec —— 无论是否真的归还回 pool,都对应一次 Acquire 的释放。
    // 顺序:先尝试归还到 pool(若 backend 仍 open),最后 dec。
    //
    // closed_ 的 fast-path:如果 backend 已 Close,pool 状态已被 munmap,
    // 此时不能再触碰 freeStack_/poolBase_,直接跳过推回逻辑只 dec 即可。

    // Fast-path:一次 atomic load,避免无谓加锁
    if (closed_.load(std::memory_order_acquire)) {
        outstanding_count_.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        // 锁内 recheck:Close 拿锁前会先 set closed_,我们持锁后再读一次能
        // 避免 use-after-free 的 race 窗口。
        if (!closed_.load(std::memory_order_acquire)) {
            // Double-release 检测:同 buffer_pool::Release 的逻辑。slot 可能因
            // 上层 Engine 的 bug 重复释放,放过去会让两次 Acquire 拿到同 slot
            // → 静默 corruption。O(N) 扫描,N = bufferPoolCount(默认 8)开销可忽略。
            for (const std::uint32_t existing : freeStack_) {
                if (existing == impl.slot_index_) {
                    CABE_LOG_ERROR("SyncIoBackend::ReturnBuffer_Internal: "
                                   "double release detected, slot=%u",
                                   impl.slot_index_);
                    // 不推回栈,不让 corruption 发生。但 outstanding_count_ 仍 dec
                    // (这次"释放"在调用方视角是已发生的)。
                    outstanding_count_.fetch_sub(1, std::memory_order_acq_rel);
                    return;
                }
            }
            // capacity 在 Open 时已 reserve(bufferPoolCount_),push_back 对
            // uint32_t 是 noexcept,不会抛。
            freeStack_.push_back(impl.slot_index_);
        }
    }
    outstanding_count_.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace cabe::io
