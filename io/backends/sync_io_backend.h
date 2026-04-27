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
#include <string>

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

    // ===== 生命周期 =====
    int32_t       Open(const std::string& devicePath, std::uint32_t bufferPoolCount);
    int32_t       Close();
    bool          IsOpen()      const noexcept;
    std::uint64_t BlockCount()  const noexcept;

    // ===== Buffer 生命周期 =====
    // 池耗尽时返回 invalid handle(Q3,调用方靠 .valid() 检查)。
    // 归还由 BufferHandle 析构经 BufferHandleImpl::~ → ReturnBuffer_Internal(私有)
    // 完成,无显式 Release 方法(Q1 = RAII)。
    BufferHandle AcquireBuffer();

    // ===== 块 I/O =====
    int32_t WriteBlock(BlockId blockId, const BufferHandle& handle);
    int32_t ReadBlock (BlockId blockId, BufferHandle&       handle);

    // ===== Q7 支持 =====
    // BufferHandleImpl 析构时查询。已 Close 则归还逻辑应 no-op 退出
    // (backend 资源已释放,继续访问会 UAF)。
    bool is_closed() const noexcept;

private:
    // BufferHandleImpl 析构调它把自己归还到池。Impl 是 friend,
    // 这样可以 keep ReturnBuffer_Internal 私有,不暴露给 Engine。
    friend class BufferHandleImpl;
    void ReturnBuffer_Internal(BufferHandleImpl& impl) noexcept;

    // ---------------------------------------------------------------
    // M1 阶段:成员仅占位。M2 引入 fd_ / mmap base / freeStack_ /
    // stackMutex_ 等真实状态。
    // ---------------------------------------------------------------
    bool                       opened_            = false;
    std::atomic<bool>          closed_            {false};
    std::atomic<std::uint32_t> outstanding_count_ {0};
};

} // namespace cabe::io

#endif // CABE_IO_BACKENDS_SYNC_IO_BACKEND_H
