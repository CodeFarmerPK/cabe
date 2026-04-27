/*
 * Project: Cabe
 * Created Time: 2026-04-27
 * Created by: CodeFarmerPK
 *
 * SyncIoBackend —— P3 sync IoBackend 后端声明(M1 骨架)。
 *
 * M1 状态:仅给出类形状 + 方法签名,内部为 stub(Open / Read / Write 返回
 * IO_BACKEND_NOT_OPEN;AcquireBuffer 返回 invalid handle)。M2 阶段把
 * storage/storage.cpp 与 buffer/buffer_pool.cpp 的真实逻辑迁入此文件,
 * 接口签名不变。
 *
 * 后端形态(M2 起生效):
 *   - 设备:O_DIRECT | O_SYNC | O_RDWR 打开裸块设备
 *           (fstat + S_ISBLK + ioctl(BLKGETSIZE64) 校验)
 *   - Buffer pool:mmap(MAP_ANONYMOUS) 分一大块,内部 LIFO slot
 *   - 块 I/O:pread / pwrite at blockId * CABE_VALUE_DATA_SIZE
 *
 * 线程安全(P3 范围):
 *   - Open / Close 由调用方(Engine)串行
 *   - AcquireBuffer / handle 析构归还 由 backend 内部锁保护
 *   - ReadBlock / WriteBlock 在 fd 上不同 offset 并发安全(pread/pwrite 语义)
 *
 * Q7 配套:
 *   - 维护 outstanding_count_ 原子计数
 *   - Close 检查计数:Debug 不为 0 abort,Release 警告 + 强制释放
 *   - is_closed() 给 BufferHandleImpl::~ 在 Close 后 no-op 退出用
 */

#ifndef CABE_IO_BACKENDS_SYNC_IO_BACKEND_H
#define CABE_IO_BACKENDS_SYNC_IO_BACKEND_H

#include "common/error_code.h"
#include "common/structs.h"
#include "io/buffer_handle.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>


namespace cabe::io {

class BufferHandleImpl;     // 完整定义见 sync_buffer_handle_impl.h

class SyncIoBackend {
public:
    SyncIoBackend()  = default;
    ~SyncIoBackend();

    SyncIoBackend(const SyncIoBackend&)            = delete;
    SyncIoBackend& operator=(const SyncIoBackend&) = delete;
    SyncIoBackend(SyncIoBackend&&)                 = delete;
    SyncIoBackend& operator=(SyncIoBackend&&)      = delete;

    // ===== 生命周期(状态机为终态 Close)=====
    //   (opened=false,closed=false) ──Open──▶ (opened=true,closed=false)
    //                                              │
    //                                            Close
    //                                              ▼
    //                              (opened=false,closed=true)   ← terminal
    //
    // Open 失败/二次调用 → IO_BACKEND_ALREADY_OPEN 或具体 DEVICE_*/POOL_* 码
    // Close 在 (opened=false) 状态下幂等 SUCCESS、不进入 terminal
    // Close 在 (opened=true) 时进入 terminal,后续 Open 一律 IO_BACKEND_ALREADY_OPEN
    // 想"重新打开"必须销毁实例构造新的(与 Engine usedOnce_ 一致)
    int32_t       Open(const std::string& devicePath, std::uint32_t bufferPoolCount);
    int32_t       Close();
    bool          IsOpen()      const noexcept;
    std::uint64_t BlockCount()  const noexcept;

    // ===== Buffer 生命周期 =====
    // 池耗尽时返回 invalid handle(Q3,调用方靠 .valid() 检查)。
    // 归还由 BufferHandle 析构经 BufferHandleImpl::~ → ReturnBuffer_Internal(私有)
    // 完成,无显式 Release 方法(Q1 = RAII)。
    //
    // Q2:发放的 buffer **内容未定义**,不做 memset。调用方(Engine::Put 小 value 分支)
    // 必须自行覆盖或补尾。
    BufferHandle AcquireBuffer();

    // ===== 块 I/O(阻塞到完成,线程安全)=====
    // handle 必须是本实例 AcquireBuffer 发放的 valid handle;否则
    // 返回 IO_BACKEND_INVALID_HANDLE。
    int32_t WriteBlock(BlockId blockId, const BufferHandle& handle);
    int32_t ReadBlock (BlockId blockId, BufferHandle&       handle);

    // ===== Q7 支持 =====
    // BufferHandleImpl 析构时查询。已 Close 则归还逻辑应 no-op 退出
    // (backend 资源已释放,继续访问会 UAF)。
    // BufferHandleImpl 析构时查询。已 Close 则归还逻辑只 dec outstanding_count_
    // 并 no-op 退出(backend pool 资源已释放,继续访问会 UAF)。
    bool is_closed() const noexcept;

private:
    // BufferHandleImpl 析构调它把自己归还到池。Impl 是 friend,
    // 这样可以 keep ReturnBuffer_Internal 私有,不暴露给 Engine。
    friend class BufferHandleImpl;
    void ReturnBuffer_Internal(BufferHandleImpl& impl) noexcept;

    // ----------------------------------------------------------------
    // 生命周期状态(由调用方串行化保证,Engine 用 unique_lock 保护 Open/Close;
    // 只在内部 Close 时与 atomic closed_ 配合,无需独立 mutex)。
    // ----------------------------------------------------------------
    bool                       opened_            = false;
    // ----------------------------------------------------------------
    // 设备字段(从 storage::Storage 迁入)。Engine 串行化保护读写;
    // ReadBlock/WriteBlock 在不同 offset 上 fd 并发是 pread/pwrite 内置安全。
    // ----------------------------------------------------------------
    int                        fd_                = -1;
    std::string                devicePath_;
    std::uint64_t              blockCount_        = 0;

    // ----------------------------------------------------------------
    // 多线程访问字段(Q7 + 多线程 I/O)
    //   closed_           : 一旦 Close 真正生效(opened_ 曾 true)即设 true,
    //                       永不复位。BufferHandleImpl::~ 用 atomic 读快速判断。
    //   outstanding_count_: 已 Acquire 但尚未析构的 handle 数。
    //                       AcquireBuffer ++,ReturnBuffer_Internal --。
    // ----------------------------------------------------------------
    std::atomic<bool>          closed_            {false};
    std::atomic<std::uint32_t> outstanding_count_ {0};
    // ----------------------------------------------------------------
    // Pool 状态(从 buffer::BufferPool 迁入,poolMutex_ 保护)。
    //   poolBase_         : mmap 返回的基地址,1 MiB 切片
    //   poolBufferCount_  : slot 总数
    //   poolTotalSize_    : poolBufferCount_ * CABE_VALUE_DATA_SIZE
    //   freeStack_        : 倒序压入的空闲 slot 索引(LIFO)
    // bufferSize 不再可配:SyncIoBackend 永远使用 CABE_VALUE_DATA_SIZE。
    // ----------------------------------------------------------------
    mutable std::mutex         poolMutex_;
    char*                      poolBase_          = nullptr;
    std::uint32_t              poolBufferCount_   = 0;
    std::size_t                poolTotalSize_     = 0;
    std::vector<std::uint32_t> freeStack_;
};

} // namespace cabe::io

#endif // CABE_IO_BACKENDS_SYNC_IO_BACKEND_H
