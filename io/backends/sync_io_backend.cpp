/*
 * Project: Cabe
 * Created Time: 2026-04-27
 * Created by: CodeFarmerPK
 *
 * SyncIoBackend P3 M1 实现 —— stub。
 *
 * 本文件只提供能让骨架编译通过、concept 满足、PIMPL 闭环的最小实现:
 *   - Open / Read / Write 全部返回 IO_BACKEND_NOT_OPEN
 *   - Close 仅置 closed_ 标志(供 Q7 查询)
 *   - AcquireBuffer 永远返回 invalid handle(默认构造)
 *   - 无 fd、无 mmap、无 freeStack
 *
 * M2 阶段会把 storage/storage.cpp + buffer/buffer_pool.cpp 的逻辑迁入,
 * 届时方法体替换,接口签名不变。
 *
 * BufferHandleImpl::~ 也定义在此文件,因为它需要 SyncIoBackend 完整类型
 * 才能调 ReturnBuffer_Internal。把它放头文件里会引入循环依赖。
 */

#include "io/backends/sync_io_backend.h"
#include "io/backends/sync_buffer_handle_impl.h"
#include "common/error_code.h"

namespace cabe::io {

// =====================================================================
// BufferHandleImpl::~BufferHandleImpl
// 放在这里,因为需要 SyncIoBackend 完整类型(调 ReturnBuffer_Internal)。
// =====================================================================
BufferHandleImpl::~BufferHandleImpl() {
    if (owner_ == nullptr) {
        return;     // invalid handle:啥也没拿,不归还
    }
    if (owner_->is_closed()) {
        // Q7:Close 已发生,backend 资源已释放;归还动作会 UAF,直接放弃。
        // Debug build 下 SyncIoBackend::Close 已 abort(M2 实现),
        // 运行到这里只可能是 Release 模式 + force-release 路径。
        return;
    }
    owner_->ReturnBuffer_Internal(*this);
}

// =====================================================================
// SyncIoBackend
// =====================================================================

SyncIoBackend::~SyncIoBackend() {
    if (opened_) {
        (void) Close();     // M2 阶段会让 Close 检查 outstanding_count_
    }
}

int32_t SyncIoBackend::Open(const std::string& /*devicePath*/,
                            std::uint32_t /*bufferPoolCount*/) {
    // M1 stub:M2 把 storage::Open 的 fstat + S_ISBLK + open(O_DIRECT)
    // + mmap(pool) 一起搬过来。
    return IO_BACKEND_NOT_OPEN;
}

int32_t SyncIoBackend::Close() {
    // M1 stub:M2 会做 munmap + close(fd) + 检查 outstanding_count_(Q7)。
    closed_.store(true, std::memory_order_release);
    opened_ = false;
    return SUCCESS;
}

bool SyncIoBackend::IsOpen() const noexcept {
    return opened_ && !closed_.load(std::memory_order_acquire);
}

std::uint64_t SyncIoBackend::BlockCount() const noexcept {
    return 0;       // M2 填:= ioctl(BLKGETSIZE64) / CABE_VALUE_DATA_SIZE
}

BufferHandle SyncIoBackend::AcquireBuffer() {
    // M1 stub:M2 会从 freeStack 取 slot,构造 BufferHandleImpl 并 return BufferHandle{impl}。
    return BufferHandle{};      // invalid
}

int32_t SyncIoBackend::WriteBlock(BlockId /*blockId*/,
                                  const BufferHandle& /*handle*/) {
    return IO_BACKEND_NOT_OPEN;
}

int32_t SyncIoBackend::ReadBlock(BlockId /*blockId*/,
                                 BufferHandle& /*handle*/) {
    return IO_BACKEND_NOT_OPEN;
}

bool SyncIoBackend::is_closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

void SyncIoBackend::ReturnBuffer_Internal(BufferHandleImpl& /*impl*/) noexcept {
    // M1 stub:M2 把 BufferPool::Release 的逻辑迁过来 ——
    //   { lock_guard(stackMutex_); freeStack_.push_back(impl.slot_index_); }
    // 当前 stub 只递减 outstanding_count_,但 M1 的 AcquireBuffer 永远不
    // fetch_add(返回 invalid handle),所以这条路径在 M1 实际不会被触达。
    // 保留以确保 M2 接入时 ABI 不变。
    outstanding_count_.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace cabe::io
