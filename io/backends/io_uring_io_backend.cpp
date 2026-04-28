/*
 * Project: Cabe
 * Created Time: 2026-04-28
 * Created by: CodeFarmerPK
 *
 * IoUringIoBackend —— P4 io_uring 后端实现(M2 阶段)。
 *
 * M2 状态:Open / Close / AcquireBuffer / IsOpen / BlockCount / is_closed /
 *          ReturnBuffer_Internal 全部真实实现;WriteBlock / ReadBlock 仍 stub
 *          (返回 IO_BACKEND_NOT_OPEN),M3 起填实。
 *
 * M2 落地的功能:
 *   - Open: open(O_DIRECT|O_SYNC|O_RDWR) + S_ISBLK + BLKGETSIZE64 +
 *           mmap(n × 1 MiB) pool + 切片 freeStack_ + io_uring_queue_init
 *   - Open 失败的完整 rollback:任何步骤失败都精确回滚已分配资源,保证
 *     "失败 → opened_=false 且无资源残留" 的不变式
 *   - Close: closed_=true → drain in-flight CQE → Q7 outstanding 检查
 *           → queue_exit + delete ring_state_ → munmap pool → close fd
 *   - AcquireBuffer: LIFO 弹 slot + 构造 BufferHandleImpl(填 fixed_buf_index_)
 *                   + outstanding_count_++
 *   - ReturnBuffer_Internal: closed_ fast-path + double-release 检测 +
 *                           outstanding_count_--
 *
 * M2 不做的(留给后续 milestone,详见 doc/p4_io_uring_design.md §13):
 *   M3: WriteBlock / ReadBlock 真实实现(Model A,prep_write/read,无 FIXED)
 *   M4: io_uring_register_buffers + WRITE_FIXED / READ_FIXED + fixed_buf_index_ 真用
 *   M5: io_uring_register_files + IOSQE_FIXED_FILE
 *   M6: io_uring_specific_test + Options.io_uring_sq_depth + 部署文档
 *   M7: WriteBlocks / ReadBlocks 批量 API
 *
 * 与 sync 后端的实现对齐(`io/backends/sync_io_backend.cpp`):
 *   - 状态机:终态 Close,Close-before-Open 幂等 no-op
 *   - Q1 RAII 归还 / Q2 不清零 / Q3 池耗尽返回 invalid / Q7 outstanding 检查
 *   - 错误码翻译:DEVICE_* / POOL_* 在 backend 层暴露;抽象层契约见 IO_BACKEND_*
 *   - 参考 memory/project_roadmap.md Q1-Q7 决策
 *
 * io_uring 特有项(M2 阶段):
 *   - SQ depth 固定为 64(M6 起从 Options.io_uring_sq_depth 取);M2 不会
 *     submit 任何 op,depth 数值不影响功能正确性
 *   - PIMPL RingState 隐藏 liburing 类型,公开头零依赖 liburing.h
 *   - drain 循环 D17 不带超时(健壮运行环境假设);M2 阶段 in_flight_count_
 *     永远 0,循环逻辑写出来给 M3+ 自动生效
 *   - io_mutex_ 临界区覆盖 drain + queue_exit + delete ring_state_,
 *     防止 M3+ WriteBlock 等锁期间 ring_state_ 被 destroy 导致 UAF
 *     (M3 必须配套:WriteBlock / ReadBlock 在 io_mutex_ 内 recheck closed_
 *      或 ring_state_ != nullptr)
 *
 * 详细设计:doc/p4_io_uring_design.md §6、§9
 */

#include "io/backends/io_uring_io_backend.h"
#include "io/backends/io_uring_buffer_handle_impl.h"

#include "common/error_code.h"
#include "common/logger.h"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <utility>

#include <fcntl.h>
#include <linux/fs.h>       // BLKGETSIZE64
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <liburing.h>

namespace cabe::io {

namespace {

// M2 阶段 SQ depth 的硬编码默认值。M6 落地 Options.io_uring_sq_depth 后
// 从 Open 入参或 Options 传入;约束 sq_depth >= buffer_pool_count 也在 M6 校验。
// io_uring_queue_init 要求 entries 是 2 的幂(老内核硬性要求,新内核宽松);
// 64 = 2^6 满足。M2 阶段不 submit 任何 op,数值不影响功能。
constexpr unsigned kDefaultSqDepth = 64;

} // namespace

// =====================================================================
// PIMPL 内部:io_uring 真实状态。
//   ring                : liburing 主结构,Open 内 io_uring_queue_init 后有效
//                         `ring{}` value-init 零填整个结构,即便未 init,
//                         结构内字段也是已知零值,Close 路径上的 ring_initialized
//                         旁路检查更安全。
//   ring_initialized    : queue_init 是否成功的旁路标志
//                         (Close 路径用,避免对未 init 的 ring 调 queue_exit)
//   buffers_registered  : 是否已 io_uring_register_buffers(M4 起为 true)
//                         Close 用,决定是否 unregister
//   files_registered    : 是否已 io_uring_register_files(M5 起为 true)
// =====================================================================
struct IoUringIoBackend::RingState {
    io_uring ring{};
    bool     ring_initialized   = false;
    bool     buffers_registered = false;
    bool     files_registered   = false;
};

// =====================================================================
// BufferHandleImpl::~BufferHandleImpl —— 出域定义。
//
// 必须在能看到 IoUringIoBackend 完整类型的 TU 里(否则 friend 调用拒编)。
//
// owner_ == nullptr   → invalid handle / Acquire 失败时构造的 handle,
//                       啥也没拿,直接 return
// owner_ != nullptr   → 委托 ReturnBuffer_Internal:
//                       内部 closed_ fast-path / 双释放检测 / dec 计数
// =====================================================================
BufferHandleImpl::~BufferHandleImpl() {
    if (owner_ != nullptr) {
        owner_->ReturnBuffer_Internal(*this);
    }
}

// =====================================================================
// IoUringIoBackend
// =====================================================================

IoUringIoBackend::~IoUringIoBackend() {
    if (opened_) {
        // 析构无法 propagate 错误码;Close 内部对 munmap / close fd 失败会
        // 自行 log。这里只是 RAII 兜底。
        (void) Close();
    }
    // Open 失败路径已用 unique_ptr 自动清理 ring_state_;Open 成功后由 Close
    // 释放 ring_state_。此处无需额外 delete。
}

int32_t IoUringIoBackend::Open(const std::string& devicePath,
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

    // ===== 第 1 阶段:打开裸块设备(等价 sync 后端逻辑)=====
    //
    // S_ISBLK 必须前置于 open(O_DIRECT) —— Linux do_dentry_open 在 O_DIRECT
    // 但 a_ops 不实现 direct_IO 时直接 EINVAL,先 stat 再 open 才能给出
    // 准确的 DEVICE_NOT_BLOCK_DEVICE。
    struct stat st{};
    if (::stat(devicePath.c_str(), &st) < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    if (!S_ISBLK(st.st_mode)) {
        CABE_LOG_WARN("IoUringIoBackend::Open: not a block device, path=%s",
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
        CABE_LOG_ERROR("IoUringIoBackend::Open: BLKGETSIZE64 failed, errno=%d", errno);
        ::close(new_fd);
        return DEVICE_QUERY_FAILED;
    }
    const std::uint64_t newBlockCount = devBytes / CABE_VALUE_DATA_SIZE;
    if (newBlockCount == 0) {
        CABE_LOG_WARN("IoUringIoBackend::Open: device too small, bytes=%llu",
                      static_cast<unsigned long long>(devBytes));
        ::close(new_fd);
        return DEVICE_TOO_SMALL;
    }

    // ===== 第 2 阶段:mmap pool(等价 sync 后端 BufferPool::Init)=====
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

    // ===== 第 3 阶段:本地构造 freeStack / devicePath / RingState(可能 throw)=====
    //
    // 用 unique_ptr 持有 RingState,任何后续步骤异常都自动释放。
    // 本块整体由 try/catch 包裹:vector::reserve / push_back / string copy /
    // make_unique 都可能 throw bad_alloc;失败统一回滚 mmap + fd。
    std::vector<std::uint32_t>  newStack;
    std::string                 newPath;
    std::unique_ptr<RingState>  new_ring_state;
    try {
        newStack.reserve(bufferPoolCount);
        // 倒序压入,pop_back 时按地址顺序分配
        for (std::uint32_t i = bufferPoolCount; i > 0; --i) {
            newStack.push_back(i - 1);
        }
        newPath        = devicePath;
        new_ring_state = std::make_unique<RingState>();
    } catch (...) {
        ::munmap(mmap_ptr, totalSize);
        ::close(new_fd);
        // 内存分配失败,复用 POOL_MMAP_FAILED 表达"pool 相关分配/初始化失败"。
        return POOL_MMAP_FAILED;
    }

    // ===== 第 4 阶段:io_uring_queue_init =====
    //
    // 失败处置(D15 一致姿态):rollback 全部已分配资源,Open 整体失败。
    // 设计文档 §10 把 queue_init 失败映射为 IO_BACKEND_NOT_OPEN
    // (语义"backend 没开",与"未 Open 状态调用"共用)。
    //
    // 失败时 unique_ptr 析构会 free RingState 内存;io_uring 未 init 成功,
    // 不可调 io_uring_queue_exit(那会触碰未初始化字段)。
    const int qrc = ::io_uring_queue_init(kDefaultSqDepth,
                                          &new_ring_state->ring, 0);
    if (qrc < 0) {
        CABE_LOG_ERROR("IoUringIoBackend::Open: io_uring_queue_init failed, errno=%d",
                       -qrc);
        ::munmap(mmap_ptr, totalSize);
        ::close(new_fd);
        return IO_BACKEND_NOT_OPEN;
    }
    new_ring_state->ring_initialized = true;

    // ===== 第 4.5 阶段:M4 起在此处 io_uring_register_buffers =====
    //   - 失败按 D15 走整体失败:queue_exit + ring_state_ 释放 + munmap + close fd
    //   - 成功后 new_ring_state->buffers_registered = true
    // ===== 第 4.6 阶段:M5 起在此处 io_uring_register_files =====
    //   - 同样的 rollback 模式

    // ===== 第 5 阶段:全部成功,no-throw 提交 =====
    fd_              = new_fd;
    blockCount_      = newBlockCount;
    devicePath_      = std::move(newPath);
    poolBase_        = static_cast<char*>(mmap_ptr);
    poolBufferCount_ = bufferPoolCount;
    poolTotalSize_   = totalSize;
    freeStack_       = std::move(newStack);
    ring_state_      = new_ring_state.release();
    opened_          = true;
    return SUCCESS;
}

int32_t IoUringIoBackend::Close() {
    if (!opened_) {
        // Close-before-Open 或重复 Close:幂等 no-op,不进入 terminal,
        // 也不设 closed_(允许后续 Open)。与 sync 后端语义一致。
        return SUCCESS;
    }

    // ---- Q7:outstanding handle 检查(在 closed_.store 之前,与 sync 对齐)----
    //
    // Debug 路径在此处 abort,留下"未 munmap / 未 queue_exit / closed_=false"
    // 的清晰现场,便于调试。Release 路径 log warn 后继续走完整 cleanup;
    // outstanding 的 BufferHandle 析构会进入 ReturnBuffer_Internal 的
    // closed_=true fast-path,只 dec count 不触碰 pool / ring 状态。
    const std::uint32_t pending = outstanding_count_.load(std::memory_order_acquire);
    if (pending != 0) {
#ifndef NDEBUG
        CABE_LOG_ERROR("IoUringIoBackend::Close: %u outstanding handles, aborting (Debug)",
                       pending);
        std::abort();
#else
        CABE_LOG_ERROR("IoUringIoBackend::Close: %u outstanding handles, force-releasing",
                       pending);
#endif
    }

    // ---- 标记 closed_ ——必须先于 pool munmap 与 ring 销毁 ----
    //
    // 已经持有 BufferHandle 还未析构的线程,fast-path 读到 closed_=true 跳过
    // pool 推回;就算 fast-path 没读到,也会在 ReturnBuffer_Internal 拿
    // poolMutex_ 后 recheck(此时 Close 可能已 munmap,但 dec count 不需要 pool 状态)。
    //
    // 同时也是 M3+ WriteBlock 的"拒绝新提交"信号:WriteBlock 在 io_mutex_ 内
    // recheck closed_,看到 true 直接返回 IO_BACKEND_NOT_OPEN,不会再去摸
    // ring_state_(下面会被 delete)。
    closed_.store(true, std::memory_order_release);

    // ---- ring 清理 + drain(io_mutex_ 临界区一气呵成,防 M3+ WriteBlock UAF)----
    //
    // M3 必须配套:WriteBlock / ReadBlock 在 io_mutex_ 内 recheck closed_ 或
    // ring_state_ != nullptr。否则有"等锁期间 ring 被 destroy" 的 race —
    // WriteBlock 拿到 io_mutex_ 后读 ring_state_ 已是悬空指针。
    {
        std::lock_guard<std::mutex> lock(io_mutex_);

        // ---- drain in-flight CQE(D17:不带超时,健壮运行环境假设)----
        //
        // M2 阶段 in_flight_count_ 永远 0(WriteBlock / ReadBlock 仍 stub),
        // 此循环空转跳过;M3 起 WriteBlock 真实提交后此处自动生效。
        while (in_flight_count_.load(std::memory_order_acquire) > 0) {
            io_uring_cqe* cqe = nullptr;
            const int wrc = ::io_uring_wait_cqe(&ring_state_->ring, &cqe);
            if (wrc < 0) {
                // 健壮环境假设下不应发生(D17);若发生,记 log 并 break,
                // 残留 in_flight_count_ 视为不可恢复(但 Close 仍继续兜底清理)。
                CABE_LOG_ERROR("IoUringIoBackend::Close: wait_cqe failed during drain, "
                               "errno=%d", -wrc);
                break;
            }
            ::io_uring_cqe_seen(&ring_state_->ring, cqe);
            in_flight_count_.fetch_sub(1, std::memory_order_acq_rel);
        }

        // ---- (M5) io_uring_unregister_files ----
        // ---- (M4) io_uring_unregister_buffers ----
        // M2 阶段没注册,跳过对应 unregister。

        // ---- queue_exit + 释放 ring_state_ ----
        //
        // ring_initialized 旁路:Open 内 queue_init 失败时不会走到这里
        // (Open 自身 rollback 已 free RingState),Close 路径下 ring_initialized
        // 必然为 true。冗余检查只是 belt-and-braces。
        if (ring_state_ != nullptr && ring_state_->ring_initialized) {
            ::io_uring_queue_exit(&ring_state_->ring);
        }
        delete ring_state_;
        ring_state_ = nullptr;
    }

    // ---- Pool 清理(锁内,与 ReturnBuffer_Internal 互斥)----
    int32_t poolRc = SUCCESS;
    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        if (poolBase_ != nullptr) {
            if (::munmap(poolBase_, poolTotalSize_) != 0) {
                poolRc = POOL_MMAP_FAILED;
                CABE_LOG_ERROR("IoUringIoBackend::Close: munmap failed, errno=%d", errno);
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

bool IoUringIoBackend::IsOpen() const noexcept {
    return opened_ && !closed_.load(std::memory_order_acquire);
}

std::uint64_t IoUringIoBackend::BlockCount() const noexcept {
    return opened_ ? blockCount_ : 0;
}

bool IoUringIoBackend::is_closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

BufferHandle IoUringIoBackend::AcquireBuffer() {
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

    impl->ptr_             = ptr;
    impl->size_            = CABE_VALUE_DATA_SIZE;
    impl->slot_index_      = slot_index;
    // D13:n × 1 MiB iovec 一一对应,fixed_buf_index_ == slot_index_。
    // M2 阶段 register_buffers 未做(M4),fixed_buf_index_ 字段填值不被使用;
    // 提前填好让 M4 切换 *_FIXED ops 时 BufferHandleImpl 字段已就位。
    impl->fixed_buf_index_ = slot_index;
    impl->owner_           = this;

    // 计数 ++ 必须放在 BufferHandle 构造之前,以防异常导致计数与 slot 不平衡
    // (BufferHandle 接管 unique_ptr 的构造是 noexcept,这步之后必无 throw)。
    outstanding_count_.fetch_add(1, std::memory_order_acq_rel);
    return BufferHandle{std::move(impl)};
}

int32_t IoUringIoBackend::WriteBlock(BlockId, const BufferHandle&) {
    // M2 stub:Open 已可成功,但 I/O 路径未实现。M3 起真实实现:
    //   1. 检查 closed_ / handle.valid / blockId 范围
    //   2. lock(io_mutex_); recheck closed_ / ring_state_ != nullptr; in_flight++
    //   3. sqe = io_uring_get_sqe(&ring_state_->ring)
    //   4. M3:io_uring_prep_write(sqe, fd_, ptr, size, offset);
    //      M4:io_uring_prep_write_fixed(sqe, fd_idx, ptr, size, offset, fixed_buf_idx)
    //      M5:sqe->flags |= IOSQE_FIXED_FILE; sqe->fd = 0
    //   5. io_uring_submit_and_wait(&ring_state_->ring, 1)
    //   6. cqe = io_uring_peek_cqe(...); 读 cqe->res; io_uring_cqe_seen(...)
    //   7. in_flight--; 错误映射(SUBMIT_FAILED / IO_FAILED / 短读短写)
    return IO_BACKEND_NOT_OPEN;
}

int32_t IoUringIoBackend::ReadBlock(BlockId, BufferHandle&) {
    // M2 stub。M3 起真实实现(对称 WriteBlock,prep_read / prep_read_fixed)。
    return IO_BACKEND_NOT_OPEN;
}

void IoUringIoBackend::ReturnBuffer_Internal(BufferHandleImpl& impl) noexcept {
    // outstanding_count_ 永远 dec —— 无论是否真的归还回 pool,都对应一次
    // Acquire 的释放。顺序:先尝试归还到 pool(若 backend 仍 open),最后 dec。
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
            // Double-release 检测:slot 可能因上层 Engine 的 bug 重复释放,
            // 放过去会让两次 Acquire 拿到同 slot → 静默 corruption。
            // O(N) 扫描,N = bufferPoolCount(默认 8)开销可忽略。
            for (const std::uint32_t existing : freeStack_) {
                if (existing == impl.slot_index_) {
                    CABE_LOG_ERROR("IoUringIoBackend::ReturnBuffer_Internal: "
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
