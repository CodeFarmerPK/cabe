/*
 * Project: Cabe
 * Created Time: 2026-04-28
 * Created by: CodeFarmerPK
 *
 * IoUringIoBackend —— P4 io_uring 后端声明(M1 骨架)。
 *
 * M1 状态:仅给出类形状 + 方法签名,内部全部 stub:
 *   - Open / Read / Write 返回 IO_BACKEND_NOT_OPEN
 *   - Close 在未 Open 上幂等返回 SUCCESS(与 sync 后端语义对齐)
 *   - AcquireBuffer 返回 invalid handle(等同 Q3 池耗尽契约)
 *   - IsOpen / BlockCount 返回默认 false / 0
 *
 * M2-M9 逐步落地(详见 doc/p4_io_uring_design.md §13):
 *   M2 — Open/Close + buffer pool + ring 真实实现(状态机 + Q7 行为对齐 sync)
 *   M3 — 朴素 WriteBlock/ReadBlock(Model A,prep_write/prep_read,无 FIXED)
 *   M4 — io_uring_register_buffers + WRITE_FIXED/READ_FIXED + fixed_buf_index_
 *   M5 — io_uring_register_files + IOSQE_FIXED_FILE
 *   M6 — Close drain 契约 + io_uring 专属测试 + Options.io_uring_sq_depth
 *   M7 — 内部 batch API(WriteBlocks/ReadBlocks)+ Engine 多 chunk 路径接入
 *   M8 — (可选)Model A → Model B 升级评估(reaper 线程)
 *
 * 后端形态(M2+ 落地后):
 *   - 设备:O_DIRECT | O_SYNC | O_RDWR 打开裸块设备(同 sync 后端)
 *   - Buffer pool:mmap(MAP_ANONYMOUS) 一大块,LIFO slot;M4 起整片
 *                 register 为 io_uring fixed buffer(n × 1 MiB iovec)
 *   - Ring:io_uring_queue_init(sq_depth, ...);M5 起加 register_files
 *   - 提交模型:Model A(粗 io_mutex_ + submit_and_wait(1)),M7/M8 视情况升级
 *
 * 线程安全(P4 范围):
 *   - Open / Close 由调用方(Engine)串行
 *   - AcquireBuffer / handle 析构归还 由 backend 内部 poolMutex_ 保护
 *   - WriteBlock / ReadBlock 多线程并发提交时由 io_mutex_ 串行(Model A)
 *
 * Q7 配套(沿用 P3 sync 后端约定):
 *   - 维护 outstanding_count_ 原子计数(BufferHandle 数)
 *   - 维护 in_flight_count_ 原子计数(P4 新增,跟踪未消费的 CQE)
 *   - Close drain 等 in_flight_count_ → 0 才反注册并 queue_exit
 *   - Close 检查 outstanding_count_:Debug 不为 0 abort,Release 警告 + 强制释放
 *   - is_closed() 给 BufferHandleImpl::~ 在 Close 后 no-op 退出用
 *
 * PIMPL 隐藏 liburing:
 *   - 本头不 #include <liburing.h>,避免 Engine 等下游 TU 强依赖 liburing 头
 *   - io_uring 真实状态(io_uring 结构体、register 标志位)放在内部 struct
 *     RingState,只在 io_uring_io_backend.cpp 里通过 #include <liburing.h> 暴露
 *
 * 详细设计:doc/p4_io_uring_design.md
 */

#ifndef CABE_IO_BACKENDS_IO_URING_IO_BACKEND_H
#define CABE_IO_BACKENDS_IO_URING_IO_BACKEND_H

#include "common/error_code.h"
#include "common/structs.h"
#include "io/buffer_handle.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cabe::io {

class BufferHandleImpl;     // 完整定义见 io_uring_buffer_handle_impl.h

class IoUringIoBackend {
public:
    IoUringIoBackend()  = default;
    ~IoUringIoBackend();

    IoUringIoBackend(const IoUringIoBackend&)            = delete;
    IoUringIoBackend& operator=(const IoUringIoBackend&) = delete;
    IoUringIoBackend(IoUringIoBackend&&)                 = delete;
    IoUringIoBackend& operator=(IoUringIoBackend&&)      = delete;

    // ===== 生命周期(状态机为终态 Close,与 sync 后端语义对齐)=====
    //   (opened=false,closed=false) ──Open──▶ (opened=true,closed=false)
    //                                              │
    //                                            Close
    //                                              ▼
    //                              (opened=false,closed=true)   ← terminal
    //
    // M1 stub:Open 返回 IO_BACKEND_NOT_OPEN(暂未实现真实路径);
    //          Close 在未 Open 上幂等 SUCCESS,不进入 terminal,
    //          允许 M2 起在同实例上首次 Open 成功。
    int32_t       Open(const std::string& devicePath, std::uint32_t bufferPoolCount);
    int32_t       Close();
    bool          IsOpen()      const noexcept;
    std::uint64_t BlockCount()  const noexcept;

    // ===== Buffer 生命周期 =====
    // M1 stub:返回 invalid handle(契约上同 Q3 池耗尽路径)。
    // M2 起从 freeStack_ 弹 slot,构造 BufferHandleImpl(填 fixed_buf_index_)。
    BufferHandle AcquireBuffer();

    // ===== 块 I/O(阻塞到完成,线程安全)=====
    // M1 stub:返回 IO_BACKEND_NOT_OPEN
    // M3 起真实实现(Model A);M4 起改用 *_FIXED;M5 起加 IOSQE_FIXED_FILE
    int32_t WriteBlock(BlockId blockId, const BufferHandle& handle);
    int32_t ReadBlock (BlockId blockId, BufferHandle&       handle);

    // ===== Q7 支持 =====
    bool is_closed() const noexcept;

private:
    // BufferHandleImpl 析构调它把自己归还到池。Impl 是 friend,
    // ReturnBuffer_Internal 保持私有,不暴露给 Engine。
    friend class BufferHandleImpl;
    void ReturnBuffer_Internal(BufferHandleImpl& impl) noexcept;

    // ----------------------------------------------------------------
    // 生命周期状态(M1 stub 阶段全部默认值)
    // ----------------------------------------------------------------
    bool                       opened_            = false;

    // ----------------------------------------------------------------
    // 设备字段(M2 起填实)
    // ----------------------------------------------------------------
    int                        fd_                = -1;
    std::string                devicePath_;
    std::uint64_t              blockCount_        = 0;

    // ----------------------------------------------------------------
    // io_uring 状态:PIMPL 包裹,定义在 .cpp 里,本头不暴露 liburing.h
    //   ring_state_       : 指向内部 RingState(包含 io_uring 结构体 + 注册状态)
    //                       M1 stub 阶段保持 nullptr;M2 起在 Open 内 new
    //   io_mutex_         : Model A 多线程提交串行化(M3 起生效)
    //   in_flight_count_  : 已 submit 但未 cqe_seen 的 op 数;Close drain 用(M2 起)
    // ----------------------------------------------------------------
    struct RingState;
    RingState*                 ring_state_        = nullptr;
    mutable std::mutex         io_mutex_;
    std::atomic<std::uint32_t> in_flight_count_   {0};

    // ----------------------------------------------------------------
    // Q7 多线程跟踪(同 sync 后端语义)
    //   closed_            : Close 真正生效后置 true,永不复位
    //   outstanding_count_ : 已 Acquire 但未 dtor 的 BufferHandle 数
    // ----------------------------------------------------------------
    std::atomic<bool>          closed_            {false};
    std::atomic<std::uint32_t> outstanding_count_ {0};

    // ----------------------------------------------------------------
    // Pool 状态(M2 起 mmap + 切片;M4 起 io_uring_register_buffers)
    //   poolBase_         : mmap 基地址,n × 1 MiB 切片
    //   poolBufferCount_  : slot 总数(== Open 入参 bufferPoolCount)
    //   poolTotalSize_    : poolBufferCount_ * CABE_VALUE_DATA_SIZE
    //   freeStack_        : LIFO 空闲 slot 索引
    // ----------------------------------------------------------------
    mutable std::mutex         poolMutex_;
    char*                      poolBase_          = nullptr;
    std::uint32_t              poolBufferCount_   = 0;
    std::size_t                poolTotalSize_     = 0;
    std::vector<std::uint32_t> freeStack_;
};

} // namespace cabe::io

#endif // CABE_IO_BACKENDS_IO_URING_IO_BACKEND_H
