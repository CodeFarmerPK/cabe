/*
 * Project: Cabe
 * Created Time: 2026-04-28
 * Created by: CodeFarmerPK
 *
 * IoUringIoBackend —— P4 io_uring 后端实现(M7 已落地,M8 评估待启动)。
 *
 * 截至 M7 状态:Open / Close / AcquireBuffer / IsOpen / BlockCount / is_closed /
 *          ReturnBuffer_Internal / WriteBlock / ReadBlock / WriteBlocks /
 *          ReadBlocks 全部真实实现。
 *          单笔 WriteBlock / ReadBlock 走 Model A(粗 io_mutex_ +
 *          io_uring_prep_write_fixed / prep_read_fixed + submit_and_wait(1)),
 *          fd 已 registered + SQE 用 IOSQE_FIXED_FILE + fd_idx=0 提交。
 *          M7 批量 WriteBlocks / ReadBlocks 同 Model A 锁,但一次锁内
 *          prep N 个 SQE → submit_and_wait(N) → io_uring_for_each_cqe
 *          + cq_advance(N),省 (N-1) 次 syscall 来回。
 *          SQ depth 从 Options.io_uring_sq_depth 取(D7),Open 校验
 *          sq_depth >= pool_count(R12)且是 2 的幂(D7)。
 *
 * M7 落地的功能(M6 基础上新增):
 *   - IoBackendTraits concept 加 WriteBlocks/ReadBlocks 签名,sync 与 io_uring
 *     两类后端都实现。span<pair<BlockId, [const] BufferHandle*>> 入参,
 *     调用方持 vector,backend 不 copy/move,只读 N 项 (blockId, handlePtr)
 *   - sync 后端 WriteBlocks/ReadBlocks 退化为 for-loop 调用单笔,无额外锁,
 *     语义与多次单笔调用完全等价(D18 一致 trait 但分档优化)
 *   - io_uring 后端 WriteBlocks/ReadBlocks 真实批量:
 *       prep N 个 SQE(user_data=i) → io_uring_submit_and_wait(ring, N)
 *       → io_uring_for_each_cqe + 单次 io_uring_cq_advance(N) sweep
 *     比 N 次单笔节省 (N-1) 次 syscall(submit 1 次替代 N 次,advance 1 次替代 N 次)
 *   - Engine::Put / Get / GetIntoVector 多 chunk 路径全部切到批量 API:
 *     按 bufferPoolCount_ 分批,每批 Phase A(Acquire+memcpy+CRC)→
 *     Phase B(WriteBlocks/ReadBlocks)→ Phase C(chunkIndex.Put / 校验 CRC + memcpy)
 *   - test/io/io_uring_specific_test.cpp 新增 5 个 M7 case:
 *       - WriteBlocksReadBlocksRoundtrip(8 个 pattern + 路由不串验证)
 *       - EmptyBatchReturnsSuccess(n=0 短路 SUCCESS)
 *       - BatchRejectsNullHandle(handlePtr nullptr 返回 INVALID_HANDLE)
 *       - BatchWriteEquivalentToSerialWrites(批量与单笔写产物等价)
 *       - BatchOnNotOpenReturnsError(未 Open 时 batch 安全失败)
 *
 * M7 不做的(留给后续 milestone,详见 doc/p4_io_uring_design.md §13):
 *   M8: (可选)Model A → Model B 升级评估(reaper 线程,M7 bench 数据决定)
 *   M9: bench 归档 + README/roadmap/设计稿状态收尾
 *
 * 已有功能保留(M4-M6 兑现):
 *   - register_buffers + WRITE/READ_FIXED → buf_index 命中预注册 → 跳过 GUP/page pin
 *     bench 验证 cpu_time 加速 16–82%,远超设计稿 W4.6 的 5% 验收门
 *   - register_files + IOSQE_FIXED_FILE → 跳过 fdget/fdput(M5)
 *   - Options.io_uring_sq_depth(D7)+ R12 校验 + production deployment notes
 *   - 测试:test/io/io_uring_specific_test.cpp::RegisterBuffersFailsWhenPoolTooLarge
 *     覆盖 D15 失败路径(root 下 SKIP,见 fixture 顶部注释)
 *
 * 与 sync 后端的实现对齐(`io/backends/sync_io_backend.cpp`):
 *   - 状态机:终态 Close,Close-before-Open 幂等 no-op
 *   - Q1 RAII 归还 / Q2 不清零 / Q3 池耗尽返回 invalid / Q7 outstanding 检查
 *   - 入口校验路径返回相同的 DEVICE_NO_SPACE / IO_BACKEND_INVALID_HANDLE 等;
 *     io_uring 特有的 submit / cqe 错误用 IO_BACKEND_SUBMIT_FAILED / IO_FAILED
 *     (sync 在底层用 pwrite,失败用 DEVICE_FAILED_TO_WRITE_DATA;两者经
 *      TranslateStatus 统一映射到 cabe::Status::IOError)
 *   - 批量 API:sync 后端 for-loop 等价于 N 次单笔,io_uring 后端
 *     真实批量;两者语义一致,Engine 调用方一套代码两后端通吃
 *   - 参考 memory/project_roadmap.md Q1-Q7 决策
 *
 * io_uring 特有项(截至 M7):
 *   - SQ depth 从 Options.io_uring_sq_depth 取(M6),Open 校验 sq>=pool 且 2 幂
 *   - registered file(M5):Open 内 register_files 一次,所有 sqe 用
 *     IOSQE_FIXED_FILE + fd_idx=0 提交;Close drain 之后 unregister_files
 *     先于 unregister_buffers
 *   - registered buffer(M4):Open 内 register_buffers,WriteBlock/Blocks
 *     与 ReadBlock/Blocks 用 prep_*_fixed + buf_index = slot_index 提交
 *   - PIMPL RingState 隐藏 liburing 类型,公开头零依赖 liburing.h
 *   - Model A:io_mutex_ 全程持锁(单笔 + 批量统一)。WriteBlock / ReadBlock /
 *     WriteBlocks / ReadBlocks 入口先 fast-path 检查 closed_,锁内 recheck
 *     closed_ 再访问 ring_state_,防止与 Close 的 race UAF(Close 拿 io_mutex_
 *     后会 destroy ring_state_)
 *   - drain 循环 D17 不带超时(健壮运行环境假设);Model A 单笔 / 批量都在
 *     退出 io_mutex_ 临界区前归 0 in_flight_count_,Close drain 仍是 no-op;
 *     M8 Model B 才会真正利用 drain 循环
 *
 * 详细设计:doc/p4_io_uring_design.md §6、§8、§9、§10
 */

#include "io/backends/io_uring_io_backend.h"
#include "io/backends/io_uring_buffer_handle_impl.h"

#include "common/error_code.h"
#include "common/logger.h"

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <span>
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

// M2-M5 阶段 SQ depth 的硬编码默认值。M6 起从 Options.io_uring_sq_depth 取,
// 并加 sq_depth >= buffer_pool_count 的 Open 前置校验(R12)。
// io_uring_queue_init 要求 entries 是 2 的幂(老内核硬性要求,新内核宽松);
// 64 = 2^6 满足。Model A 一次只发 1 个 op,depth 数值不影响功能正确性,
// 只影响 M7 batch 启用后可同时 in-flight 的上限。
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
                               const std::uint32_t bufferPoolCount,
                               const std::uint32_t sqDepth) {
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

    // ---- M6 / D7 / R12:sqDepth 校验 ----
    //
    // R12:sqDepth 必须 >= bufferPoolCount。M7 batch 上线后,一次同时
    // in-flight 的 op 上限是 bufferPoolCount(因为每个 op 占一个 buffer
    // slot),sqDepth 必须容得下。Model A 当前 1:1 串行不会撞此上限,但
    // 提前校验避免 M7 上线后才发现配置错误。
    //
    // D7:sqDepth 必须是 2 的幂。io_uring_queue_init 旧内核(< 5.10)硬性
    // 要求,新内核宽松但内部仍向上对齐到 2 的幂(浪费 entries)。统一
    // 锁紧约束便于跨版本一致行为。0 也不是 2 的幂,自然被拦下。
    //
    // 上限:bufferPoolCount 隐含的 IORING_MAX_REG_BUFFERS = 16384 由
    // register_buffers 自身校验(M4 实测路径);此处不再前置校验。
    if (sqDepth < bufferPoolCount) {
        CABE_LOG_WARN("IoUringIoBackend::Open: sq_depth=%u < pool_count=%u (R12)",
                      sqDepth, bufferPoolCount);
        return POOL_INVALID_PARAMS;
    }
    if (sqDepth == 0 || (sqDepth & (sqDepth - 1)) != 0) {
        CABE_LOG_WARN("IoUringIoBackend::Open: sq_depth=%u not a power of two (D7)",
                      sqDepth);
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
    std::vector<iovec>          iovecs;     // M4: register_buffers 入参,n × {slot_ptr, 1 MiB}
    try {
        newStack.reserve(bufferPoolCount);
        // 倒序压入,pop_back 时按地址顺序分配
        for (std::uint32_t i = bufferPoolCount; i > 0; --i) {
            newStack.push_back(i - 1);
        }
        newPath        = devicePath;
        new_ring_state = std::make_unique<RingState>();

        // M4 / D13:把 mmap pool 切成 n 个独立 iovec,与 freeStack slot 一一对应。
        // 注册成功后,每个 BufferHandleImpl.fixed_buf_index_ == slot_index_
        // (AcquireBuffer 在 M2 已提前填好,M4 直接复用)。
        iovecs.reserve(bufferPoolCount);
        for (std::uint32_t i = 0; i < bufferPoolCount; ++i) {
            iovecs.push_back(iovec{
                .iov_base = static_cast<char*>(mmap_ptr) +
                            static_cast<std::size_t>(i) * CABE_VALUE_DATA_SIZE,
                .iov_len  = CABE_VALUE_DATA_SIZE
            });
        }
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
    //
    // M6 起 sqDepth 从入参 / Options.io_uring_sq_depth 取,M2-M5 阶段曾经
    // 硬编码为 kDefaultSqDepth = 64。
    const int qrc = ::io_uring_queue_init(sqDepth,
                                          &new_ring_state->ring, 0);
    if (qrc < 0) {
        CABE_LOG_ERROR("IoUringIoBackend::Open: io_uring_queue_init failed, errno=%d",
                       -qrc);
        ::munmap(mmap_ptr, totalSize);
        ::close(new_fd);
        return IO_BACKEND_NOT_OPEN;
    }
    new_ring_state->ring_initialized = true;

    // ===== 第 4.5 阶段:io_uring_register_buffers (M4 / D5 / D13 / D15)=====
    //
    // 把 stage 3 构造的 n × 1 MiB iovec 一次性注册为 io_uring fixed buffer。
    // 注册成功后,WriteBlock / ReadBlock 用 prep_*_fixed + buf_index 提交,
    // 内核跳过每次 I/O 的 GUP(get_user_pages)与 page pin/unpin 开销。
    //
    // D15 失败处置(整体失败,不 fallback 到非 FIXED 路径):
    //   - rollback:queue_exit → unique_ptr 释放 RingState → munmap → close fd
    //   - 最常见触发是 RLIMIT_MEMLOCK 撞限(默认 64 KiB,默认 pool 16 MiB 直接失败)
    //   - 文档 README "Production deployment notes"(M6 加)指导用户调 ulimit
    //   - 测试覆盖:test/io/io_uring_specific_test.cpp::RegisterBuffersFailsWhenPoolTooLarge
    const int rrc = ::io_uring_register_buffers(&new_ring_state->ring,
                                                 iovecs.data(),
                                                 static_cast<unsigned>(bufferPoolCount));
    if (rrc < 0) {
        CABE_LOG_ERROR("IoUringIoBackend::Open: io_uring_register_buffers failed, errno=%d",
                       -rrc);
        ::io_uring_queue_exit(&new_ring_state->ring);   // ring 已 init,必须 exit
        ::munmap(mmap_ptr, totalSize);
        ::close(new_fd);
        // RLIMIT_MEMLOCK 撞限 → ENOMEM,翻成 POOL_MMAP_FAILED 让上层易识别;
        // 其他 errno → IO_BACKEND_NOT_OPEN(沿用 queue_init 失败的命名约定)。
        return -rrc == ENOMEM ? POOL_MMAP_FAILED : IO_BACKEND_NOT_OPEN;
    }
    new_ring_state->buffers_registered = true;

    // ===== 第 4.6 阶段:io_uring_register_files (M5 / D15)=====
    //
    // 注册 fd 让 io_uring 跳过每次 op 的 fdget / fdput;WriteBlock / ReadBlock
    // 用 IOSQE_FIXED_FILE + sqe->fd=0(registered file table 索引,我们只注册
    // 这一个 fd,所以索引固定为 0)。
    //
    // D15 失败处置(整体失败,不 fallback 到非 fixed-file 路径):
    //   rollback:unregister_buffers → queue_exit → unique_ptr 释放 RingState
    //            → munmap → close fd
    //
    // 错误码:register_files 几乎只有 EINVAL(bad fd) / EBUSY(重复注册)等
    // 编程错误类,无类似 register_buffers 的 RLIMIT_MEMLOCK 资源类失败,
    // 因此不像 ENOMEM 那样特别区分,统一 IO_BACKEND_NOT_OPEN。
    const int frc = ::io_uring_register_files(&new_ring_state->ring,
                                              &new_fd, 1);
    if (frc < 0) {
        CABE_LOG_ERROR("IoUringIoBackend::Open: io_uring_register_files failed, errno=%d",
                       -frc);
        ::io_uring_unregister_buffers(&new_ring_state->ring);   // M4 已 register
        ::io_uring_queue_exit(&new_ring_state->ring);           // ring 已 init
        ::munmap(mmap_ptr, totalSize);
        ::close(new_fd);
        return IO_BACKEND_NOT_OPEN;
    }
    new_ring_state->files_registered = true;

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

        // ---- io_uring_unregister_files (M5 起)----
        //
        // 与 unregister_buffers 同模式:io_uring_queue_exit 会隐式释放
        // registered files,这里显式 unregister 主要是为了:
        //   1) 拿到失败 errno 写日志,便于排查
        //   2) 把 files_registered 置 false,与 buffers_registered 状态机对齐
        // 失败不阻断 close 流程,继续走 unregister_buffers / queue_exit /
        // munmap / close fd。
        //
        // 反注册次序:files 先于 buffers(对称 Open 内 register_buffers 先于
        // register_files;严格说 io_uring 不强制此顺序,这只是阅读对称性)。
        if (ring_state_ != nullptr && ring_state_->files_registered) {
            const int urc = ::io_uring_unregister_files(&ring_state_->ring);
            if (urc < 0) {
                CABE_LOG_ERROR("IoUringIoBackend::Close: unregister_files failed, errno=%d",
                               -urc);
            }
            ring_state_->files_registered = false;
        }

        // ---- io_uring_unregister_buffers (M4 起)----
        //
        // 注:io_uring_queue_exit 也会隐式释放 registered buffers,这里显式
        // unregister 主要是为了:
        //   1) 拿到失败 errno 写日志,便于排查
        //   2) 把 buffers_registered 置 false,M8 Model B 阶段即便 reaper 残留对
        //      ring 的引用,buffers 状态机也是清晰的
        // 失败不阻断 close 流程,继续走 queue_exit / munmap / close fd。
        if (ring_state_ != nullptr && ring_state_->buffers_registered) {
            const int urc = ::io_uring_unregister_buffers(&ring_state_->ring);
            if (urc < 0) {
                CABE_LOG_ERROR("IoUringIoBackend::Close: unregister_buffers failed, errno=%d",
                               -urc);
            }
            ring_state_->buffers_registered = false;
        }

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
    // M4 起 Open 内 register_buffers 落地后,此字段被 WriteBlock / ReadBlock
    // 的 io_uring_prep_*_fixed 真用作 buf_index 参数,内核据此直接命中预注册
    // buffer 跳过 GUP / page pin。
    impl->fixed_buf_index_ = slot_index;
    impl->owner_           = this;

    // 计数 ++ 必须放在 BufferHandle 构造之前,以防异常导致计数与 slot 不平衡
    // (BufferHandle 接管 unique_ptr 的构造是 noexcept,这步之后必无 throw)。
    outstanding_count_.fetch_add(1, std::memory_order_acq_rel);
    return BufferHandle{std::move(impl)};
}

int32_t IoUringIoBackend::WriteBlock(const BlockId blockId,
                                      const BufferHandle& handle) {
    // ---- 入口校验(无需 io_mutex_,字段在 opened_ 之后到 Close 前不变)----
    if (!opened_ || closed_.load(std::memory_order_acquire)) {
        return IO_BACKEND_NOT_OPEN;
    }
    if (!handle.valid()) {
        return IO_BACKEND_INVALID_HANDLE;
    }
    if (handle.size() != CABE_VALUE_DATA_SIZE) {
        return CABE_INVALID_DATA_SIZE;
    }

    // 验证 handle 来自本实例的 pool(防跨实例 / 跨 backend 误用)。
    // 不持 poolMutex_:poolBase_ / poolTotalSize_ 在 opened_ 之后到 Close 之前
    // 都不变,且本路径的 opened_ 检查已通过,这里读是安全的。
    const char* const dataPtr = handle.data();
    if (dataPtr < poolBase_ || dataPtr >= poolBase_ + poolTotalSize_) {
        return IO_BACKEND_INVALID_HANDLE;
    }

    // 越界保护:正常路径上 FreeList(在 Engine 层)已按 blockCount_ 上限拦住,
    // 此处是最后防线。返回与 sync 后端一致的 DEVICE_NO_SPACE。
    if (blockId >= blockCount_) {
        return DEVICE_NO_SPACE;
    }

    const off_t offset = static_cast<off_t>(blockId) * CABE_VALUE_DATA_SIZE;

    // ---- Model A:io_mutex_ 全程持锁,prep + submit + wait + cqe_seen 串行 ----
    std::lock_guard<std::mutex> lock(io_mutex_);

    // 锁内 recheck closed_:Close 拿到 io_mutex_ 之后会 destroy ring_state_,
    // 我们必须在锁内确认 closed_ 仍是 false,否则后面访问 ring_state_ → UAF。
    // (race 场景:本线程入口 fast-path 已通过,但还没拿到 io_mutex_ 之前
    //  Close 已经把 closed_ 设为 true 并在排队等 io_mutex_。这里 recheck 后
    //  返回 NOT_OPEN,Close 拿到锁后做清理,无 UAF。)
    if (closed_.load(std::memory_order_acquire)) {
        return IO_BACKEND_NOT_OPEN;
    }

    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
    if (sqe == nullptr) {
        // SQ 满。M3 一次只发 1 个 op + Model A 串行,SQ depth 64 ≫ 1,理论
        // 不应发生;一旦观察到说明 ring 状态异常,直接报 SUBMIT_FAILED。
        return IO_BACKEND_SUBMIT_FAILED;
    }

    // M5:io_uring_prep_write_fixed + IOSQE_FIXED_FILE。
    //   - buf_index 命中预注册的 buffer → 跳过 GUP / page pin(M4 红利)
    //   - fd 字段填 0(registered file table 索引,Open 内只 register 了一个 fd,
    //     所以索引固定 0);flags 设 IOSQE_FIXED_FILE 让内核走 registered fd
    //     快路径 → 跳过 fdget / fdput(M5 红利)
    //
    // D13 / Q1:fixed_buf_index_ == slot_index_,AcquireBuffer 时已填好,
    // 这里直接读 BufferHandleImpl 的字段(IoUringIoBackend 是 BufferHandle
    // 的 friend,可访问私有 impl_)。
    const std::uint32_t fixed_buf_idx = handle.impl_->fixed_buf_index_;
    ::io_uring_prep_write_fixed(sqe, /*fd_idx=*/0, dataPtr, CABE_VALUE_DATA_SIZE,
                                static_cast<__u64>(offset),
                                static_cast<int>(fixed_buf_idx));
    sqe->flags    |= IOSQE_FIXED_FILE;
    sqe->user_data = 0;     // Model A 1:1 不使用 user_data;M7 batch 起填下标

    in_flight_count_.fetch_add(1, std::memory_order_acq_rel);

    // 提交并等待:submit_and_wait(ring, 1) = "submit pending + 等到至少 1 个 cqe"。
    // EAGAIN 退避一次重试(设计文档 §10)。
    int submitted = ::io_uring_submit_and_wait(&ring_state_->ring, 1);
    if (submitted == -EAGAIN) {
        submitted = ::io_uring_submit_and_wait(&ring_state_->ring, 1);
    }
    if (submitted < 0) {
        in_flight_count_.fetch_sub(1, std::memory_order_acq_rel);
        return IO_BACKEND_SUBMIT_FAILED;
    }

    // submit_and_wait(1) 已确保至少 1 个 cqe ready,peek 正常情况不应失败。
    io_uring_cqe* cqe = nullptr;
    const int peeked = ::io_uring_peek_cqe(&ring_state_->ring, &cqe);
    if (peeked < 0 || cqe == nullptr) {
        in_flight_count_.fetch_sub(1, std::memory_order_acq_rel);
        return IO_BACKEND_IO_FAILED;
    }

    const int32_t res = cqe->res;
    ::io_uring_cqe_seen(&ring_state_->ring, cqe);
    in_flight_count_.fetch_sub(1, std::memory_order_acq_rel);

    if (res < 0) {
        CABE_LOG_ERROR("IoUringIoBackend::WriteBlock: cqe.res=%d (errno=%d)",
                       res, -res);
        return IO_BACKEND_IO_FAILED;
    }
    if (static_cast<std::size_t>(res) != CABE_VALUE_DATA_SIZE) {
        CABE_LOG_ERROR("IoUringIoBackend::WriteBlock: short write res=%d expected=%zu",
                       res, CABE_VALUE_DATA_SIZE);
        return IO_BACKEND_IO_FAILED;
    }
    return SUCCESS;
}

int32_t IoUringIoBackend::ReadBlock(const BlockId blockId, BufferHandle& handle) {
    // 入口校验 / io_mutex_ recheck / submit / wait / cqe_seen 流程与 WriteBlock
    // 完全对称,只把 io_uring_prep_write_fixed 换成 io_uring_prep_read_fixed。
    // 详细注释见 WriteBlock。M7 batch API 落地后这两个函数会一起重构成共享
    // helper,M4 阶段保持显式镜像(YAGNI / 先落地后优化)。
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

    std::lock_guard<std::mutex> lock(io_mutex_);

    if (closed_.load(std::memory_order_acquire)) {
        return IO_BACKEND_NOT_OPEN;
    }

    io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
    if (sqe == nullptr) {
        return IO_BACKEND_SUBMIT_FAILED;
    }

    // M5:io_uring_prep_read_fixed + IOSQE_FIXED_FILE(对称 WriteBlock)。
    // 详见 WriteBlock 处对 fd_idx=0 与 IOSQE_FIXED_FILE 的说明。
    const std::uint32_t fixed_buf_idx = handle.impl_->fixed_buf_index_;
    ::io_uring_prep_read_fixed(sqe, /*fd_idx=*/0, dataPtr, CABE_VALUE_DATA_SIZE,
                               static_cast<__u64>(offset),
                               static_cast<int>(fixed_buf_idx));
    sqe->flags    |= IOSQE_FIXED_FILE;
    sqe->user_data = 0;

    in_flight_count_.fetch_add(1, std::memory_order_acq_rel);

    int submitted = ::io_uring_submit_and_wait(&ring_state_->ring, 1);
    if (submitted == -EAGAIN) {
        submitted = ::io_uring_submit_and_wait(&ring_state_->ring, 1);
    }
    if (submitted < 0) {
        in_flight_count_.fetch_sub(1, std::memory_order_acq_rel);
        return IO_BACKEND_SUBMIT_FAILED;
    }

    io_uring_cqe* cqe = nullptr;
    const int peeked = ::io_uring_peek_cqe(&ring_state_->ring, &cqe);
    if (peeked < 0 || cqe == nullptr) {
        in_flight_count_.fetch_sub(1, std::memory_order_acq_rel);
        return IO_BACKEND_IO_FAILED;
    }

    const int32_t res = cqe->res;
    ::io_uring_cqe_seen(&ring_state_->ring, cqe);
    in_flight_count_.fetch_sub(1, std::memory_order_acq_rel);

    if (res < 0) {
        CABE_LOG_ERROR("IoUringIoBackend::ReadBlock: cqe.res=%d (errno=%d)",
                       res, -res);
        return IO_BACKEND_IO_FAILED;
    }
    if (static_cast<std::size_t>(res) != CABE_VALUE_DATA_SIZE) {
        CABE_LOG_ERROR("IoUringIoBackend::ReadBlock: short read res=%d expected=%zu",
                       res, CABE_VALUE_DATA_SIZE);
        return IO_BACKEND_IO_FAILED;
    }
    return SUCCESS;
}

// =====================================================================
// 批量 I/O(P4 M7):真实批量提交。
//
// 模式:io_mutex_ 全程持锁(Model A) → 锁外预校验 + 锁内 prep N 个 SQE
//   → submit_and_wait(N) → io_uring_for_each_cqe 一次 sweep → cq_advance(N)
//   → in_flight_count_ 减 N。一次 batch 把 (N-1) 次 submit + (N-1) 次
//   cq_advance 折叠掉,这是 M7 syscall 节省的主要来源。
//
// 错误语义(与 N 次单笔等价,设计文档 §9):
//   - 任一 cqe.res < 0 或 short I/O → 整批失败,返回首个非 SUCCESS 错码
//   - 已成功完成的 I/O 不回滚(失败前已写到设备的 block 留在那里;
//     Engine 在写齐 metaIndex 之前不暴露给读路径,保证对外不可见)
//   - get_sqe 失败时:把已 prep 的 i 个 SQE 提交并等齐 sweep,
//     避免脏 ring 影响下次 submit。正常路径不应到此(R12 + Engine
//     分批保证 n <= pool_count <= sq_depth)。
//
// 与单笔 WriteBlock/ReadBlock 的关系:
//   - n == 1 的批量与单笔语义完全等价,但多走一次校验循环,微开销可忽略
//   - Engine 多 chunk 路径用 WriteBlocks/ReadBlocks 统一调用,
//     单 chunk 仍走单笔 WriteBlock(避免一次 batch 启动开销)
// =====================================================================
int32_t IoUringIoBackend::WriteBlocks(
    std::span<const std::pair<BlockId, const BufferHandle*>> batch) {
    const std::size_t n = batch.size();
    if (n == 0) {
        return SUCCESS;
    }

    // ---- 入口快速校验(无需 io_mutex_,字段在 opened_→Close 之间不变)----
    if (!opened_ || closed_.load(std::memory_order_acquire)) {
        return IO_BACKEND_NOT_OPEN;
    }

    // ---- 锁外逐项预校验,避免半提交后回滚 ring ----
    for (std::size_t i = 0; i < n; ++i) {
        const auto& [blockId, handlePtr] = batch[i];
        if (handlePtr == nullptr || !handlePtr->valid()) {
            return IO_BACKEND_INVALID_HANDLE;
        }
        if (handlePtr->size() != CABE_VALUE_DATA_SIZE) {
            return CABE_INVALID_DATA_SIZE;
        }
        const char* const dataPtr = handlePtr->data();
        if (dataPtr < poolBase_ || dataPtr >= poolBase_ + poolTotalSize_) {
            return IO_BACKEND_INVALID_HANDLE;
        }
        if (blockId >= blockCount_) {
            return DEVICE_NO_SPACE;
        }
    }

    // ---- Model A:io_mutex_ 全程持锁 ----
    std::lock_guard<std::mutex> lock(io_mutex_);

    // 锁内 recheck closed_:防 Close 在等锁期间 destroy ring_state_(UAF)。
    if (closed_.load(std::memory_order_acquire)) {
        return IO_BACKEND_NOT_OPEN;
    }

    // ---- prep N 个 SQE ----
    // 任一 get_sqe 失败时:已 prep 的 i 个 SQE 在 sq_tail 中无法回滚
    // (liburing 没有 prep-undo API)。最稳的恢复路径是 submit + 等齐 sweep
    // 这 i 个,确保 ring 干净。R12 + Engine 分批已保证 n <= sq_depth,
    // 此分支正常不应触发。
    for (std::size_t i = 0; i < n; ++i) {
        io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
        if (sqe == nullptr) {
            CABE_LOG_ERROR("IoUringIoBackend::WriteBlocks: get_sqe failed at i=%zu (n=%zu)",
                           i, n);
            if (i > 0) {
                in_flight_count_.fetch_add(static_cast<std::uint32_t>(i),
                                            std::memory_order_acq_rel);
                const int sub = ::io_uring_submit_and_wait(&ring_state_->ring,
                                                            static_cast<unsigned>(i));
                if (sub >= 0) {
                    unsigned drained = 0;
                    unsigned head;
                    io_uring_cqe* dcqe = nullptr;
                    io_uring_for_each_cqe(&ring_state_->ring, head, dcqe) {
                        (void) head; (void) dcqe;
                        ++drained;
                        if (drained >= static_cast<unsigned>(i)) break;
                    }
                    ::io_uring_cq_advance(&ring_state_->ring, drained);
                    in_flight_count_.fetch_sub(drained, std::memory_order_acq_rel);
                }
            }
            return IO_BACKEND_SUBMIT_FAILED;
        }
        const auto& [blockId, handlePtr] = batch[i];
        const off_t offset = static_cast<off_t>(blockId) * CABE_VALUE_DATA_SIZE;
        const std::uint32_t fixed_buf_idx = handlePtr->impl_->fixed_buf_index_;
        const char* const dataPtr = handlePtr->data();

        // 与单笔 WriteBlock 完全一致:fd_idx=0(registered file)
        // + prep_write_fixed + IOSQE_FIXED_FILE,跳过 GUP / fdget。
        ::io_uring_prep_write_fixed(sqe, /*fd_idx=*/0, dataPtr, CABE_VALUE_DATA_SIZE,
                                    static_cast<__u64>(offset),
                                    static_cast<int>(fixed_buf_idx));
        sqe->flags    |= IOSQE_FIXED_FILE;
        // user_data = batch 索引,sweep 时方便日志定位失败的批内位置
        sqe->user_data = static_cast<__u64>(i);
    }

    in_flight_count_.fetch_add(static_cast<std::uint32_t>(n),
                                std::memory_order_acq_rel);

    // ---- 一次 submit_and_wait(N):N 个 SQE 一并提交 + 等齐 N 个 CQE ----
    int submitted = ::io_uring_submit_and_wait(&ring_state_->ring,
                                                 static_cast<unsigned>(n));
    if (submitted == -EAGAIN) {
        submitted = ::io_uring_submit_and_wait(&ring_state_->ring,
                                                static_cast<unsigned>(n));
    }
    if (submitted < 0) {
        // submit 失败:保守地视为 n 个全部未消费,dec 全量,后续 Close drain
        // 会自然收尾。正常路径不应到(EAGAIN 二次退避仍失败 = 资源问题)。
        in_flight_count_.fetch_sub(static_cast<std::uint32_t>(n),
                                    std::memory_order_acq_rel);
        return IO_BACKEND_SUBMIT_FAILED;
    }

    // ---- 一次 sweep N 个 CQE + 一次 cq_advance(N)----
    // 比 N 次 peek + cqe_seen 少 (N-1) 次 cq_advance 的 atomic store。
    // 任一 cqe.res 异常 → 记录首个错码;其他 cqe 继续 sweep 把 cq 头推齐,
    // 避免脏 ring。
    int32_t firstErr   = SUCCESS;
    unsigned cqeCount  = 0;
    unsigned head;
    io_uring_cqe* cqe = nullptr;
    io_uring_for_each_cqe(&ring_state_->ring, head, cqe) {
        (void) head;
        ++cqeCount;
        const int32_t res = cqe->res;
        if (res < 0) {
            if (firstErr == SUCCESS) {
                firstErr = IO_BACKEND_IO_FAILED;
                CABE_LOG_ERROR("IoUringIoBackend::WriteBlocks: cqe[%llu].res=%d "
                               "(errno=%d)",
                               static_cast<unsigned long long>(cqe->user_data),
                               res, -res);
            }
        } else if (static_cast<std::size_t>(res) != CABE_VALUE_DATA_SIZE) {
            if (firstErr == SUCCESS) {
                firstErr = IO_BACKEND_IO_FAILED;
                CABE_LOG_ERROR("IoUringIoBackend::WriteBlocks: cqe[%llu] short "
                               "write res=%d expected=%zu",
                               static_cast<unsigned long long>(cqe->user_data),
                               res, CABE_VALUE_DATA_SIZE);
            }
        }
        if (cqeCount >= n) break;
    }
    ::io_uring_cq_advance(&ring_state_->ring, cqeCount);
    in_flight_count_.fetch_sub(cqeCount, std::memory_order_acq_rel);

    if (cqeCount != n && firstErr == SUCCESS) {
        CABE_LOG_ERROR("IoUringIoBackend::WriteBlocks: got %u cqes, expected %zu",
                       cqeCount, n);
        firstErr = IO_BACKEND_IO_FAILED;
    }

    return firstErr;
}

int32_t IoUringIoBackend::ReadBlocks(
    std::span<const std::pair<BlockId, BufferHandle*>> batch) {
    // 与 WriteBlocks 完全对称,仅 prep_write_fixed → prep_read_fixed
    // 与日志字符串差异。详细注释见 WriteBlocks;M8/M9 视 bench 再决定
    // 是否抽公共 helper(目前两份代码各 ~120 行,显式镜像更易跟踪)。
    const std::size_t n = batch.size();
    if (n == 0) {
        return SUCCESS;
    }

    if (!opened_ || closed_.load(std::memory_order_acquire)) {
        return IO_BACKEND_NOT_OPEN;
    }

    for (std::size_t i = 0; i < n; ++i) {
        const auto& [blockId, handlePtr] = batch[i];
        if (handlePtr == nullptr || !handlePtr->valid()) {
            return IO_BACKEND_INVALID_HANDLE;
        }
        if (handlePtr->size() != CABE_VALUE_DATA_SIZE) {
            return CABE_INVALID_DATA_SIZE;
        }
        const char* const dataPtr = handlePtr->data();
        if (dataPtr < poolBase_ || dataPtr >= poolBase_ + poolTotalSize_) {
            return IO_BACKEND_INVALID_HANDLE;
        }
        if (blockId >= blockCount_) {
            return DEVICE_NO_SPACE;
        }
    }

    std::lock_guard<std::mutex> lock(io_mutex_);

    if (closed_.load(std::memory_order_acquire)) {
        return IO_BACKEND_NOT_OPEN;
    }

    for (std::size_t i = 0; i < n; ++i) {
        io_uring_sqe* sqe = ::io_uring_get_sqe(&ring_state_->ring);
        if (sqe == nullptr) {
            CABE_LOG_ERROR("IoUringIoBackend::ReadBlocks: get_sqe failed at i=%zu (n=%zu)",
                           i, n);
            if (i > 0) {
                in_flight_count_.fetch_add(static_cast<std::uint32_t>(i),
                                            std::memory_order_acq_rel);
                const int sub = ::io_uring_submit_and_wait(&ring_state_->ring,
                                                            static_cast<unsigned>(i));
                if (sub >= 0) {
                    unsigned drained = 0;
                    unsigned head;
                    io_uring_cqe* dcqe = nullptr;
                    io_uring_for_each_cqe(&ring_state_->ring, head, dcqe) {
                        (void) head; (void) dcqe;
                        ++drained;
                        if (drained >= static_cast<unsigned>(i)) break;
                    }
                    ::io_uring_cq_advance(&ring_state_->ring, drained);
                    in_flight_count_.fetch_sub(drained, std::memory_order_acq_rel);
                }
            }
            return IO_BACKEND_SUBMIT_FAILED;
        }
        const auto& [blockId, handlePtr] = batch[i];
        const off_t offset = static_cast<off_t>(blockId) * CABE_VALUE_DATA_SIZE;
        const std::uint32_t fixed_buf_idx = handlePtr->impl_->fixed_buf_index_;
        char* const dataPtr = handlePtr->data();

        ::io_uring_prep_read_fixed(sqe, /*fd_idx=*/0, dataPtr, CABE_VALUE_DATA_SIZE,
                                   static_cast<__u64>(offset),
                                   static_cast<int>(fixed_buf_idx));
        sqe->flags    |= IOSQE_FIXED_FILE;
        sqe->user_data = static_cast<__u64>(i);
    }

    in_flight_count_.fetch_add(static_cast<std::uint32_t>(n),
                                std::memory_order_acq_rel);

    int submitted = ::io_uring_submit_and_wait(&ring_state_->ring,
                                                 static_cast<unsigned>(n));
    if (submitted == -EAGAIN) {
        submitted = ::io_uring_submit_and_wait(&ring_state_->ring,
                                                static_cast<unsigned>(n));
    }
    if (submitted < 0) {
        in_flight_count_.fetch_sub(static_cast<std::uint32_t>(n),
                                    std::memory_order_acq_rel);
        return IO_BACKEND_SUBMIT_FAILED;
    }

    int32_t firstErr   = SUCCESS;
    unsigned cqeCount  = 0;
    unsigned head;
    io_uring_cqe* cqe = nullptr;
    io_uring_for_each_cqe(&ring_state_->ring, head, cqe) {
        (void) head;
        ++cqeCount;
        const int32_t res = cqe->res;
        if (res < 0) {
            if (firstErr == SUCCESS) {
                firstErr = IO_BACKEND_IO_FAILED;
                CABE_LOG_ERROR("IoUringIoBackend::ReadBlocks: cqe[%llu].res=%d "
                               "(errno=%d)",
                               static_cast<unsigned long long>(cqe->user_data),
                               res, -res);
            }
        } else if (static_cast<std::size_t>(res) != CABE_VALUE_DATA_SIZE) {
            if (firstErr == SUCCESS) {
                firstErr = IO_BACKEND_IO_FAILED;
                CABE_LOG_ERROR("IoUringIoBackend::ReadBlocks: cqe[%llu] short "
                               "read res=%d expected=%zu",
                               static_cast<unsigned long long>(cqe->user_data),
                               res, CABE_VALUE_DATA_SIZE);
            }
        }
        if (cqeCount >= n) break;
    }
    ::io_uring_cq_advance(&ring_state_->ring, cqeCount);
    in_flight_count_.fetch_sub(cqeCount, std::memory_order_acq_rel);

    if (cqeCount != n && firstErr == SUCCESS) {
        CABE_LOG_ERROR("IoUringIoBackend::ReadBlocks: got %u cqes, expected %zu",
                       cqeCount, n);
        firstErr = IO_BACKEND_IO_FAILED;
    }

    return firstErr;
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
