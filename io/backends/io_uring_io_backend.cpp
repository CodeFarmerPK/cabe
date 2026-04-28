/*
 * Project: Cabe
 * Created Time: 2026-04-28
 * Created by: CodeFarmerPK
 *
 * IoUringIoBackend —— P4 io_uring 后端实现(M1 骨架)。
 *
 * M1 状态:全部方法 stub(详见 io_uring_io_backend.h 顶部说明)。
 * 真实逻辑在 M2-M9 逐步填实,接口签名一字不改(IoBackendTraits concept 约束)。
 *
 * 唯一非 stub 的部分:
 *   - BufferHandleImpl::~BufferHandleImpl 必须在能看到 IoUringIoBackend
 *     完整类型的 TU 里出域。M1 阶段没有真实 AcquireBuffer 路径,所以
 *     owner_ 永远是 nullptr,析构走早退分支。
 *   - IoUringIoBackend::~IoUringIoBackend 释放可能持有的 RingState
 *     (M1 阶段 ring_state_ 永远是 nullptr,delete nullptr 安全)。
 *
 * RingState:PIMPL 包裹 io_uring 真实状态。本 .cpp 里通过 #include <liburing.h>
 * 暴露完整定义,头文件只有前向声明,避免 liburing.h 泄漏给 Engine 等下游 TU。
 *
 * 详细设计:doc/p4_io_uring_design.md §6、§8、§9
 */

#include "io/backends/io_uring_io_backend.h"
#include "io/backends/io_uring_buffer_handle_impl.h"

#include "common/error_code.h"
#include "common/structs.h"

#include <liburing.h>

namespace cabe::io {

// =====================================================================
// PIMPL 内部:io_uring 真实状态。
//   ring                : liburing 主结构,M2 起 io_uring_queue_init 后有效
//   ring_initialized    : queue_init 是否成功的旁路标志
//                         (Close 路径用,避免对未 init 的 ring 调 queue_exit)
//   buffers_registered  : 是否已 io_uring_register_buffers(M4 起为 true)
//                         Close 用,决定是否 unregister
//   files_registered    : 是否已 io_uring_register_files(M5 起为 true)
// =====================================================================
struct IoUringIoBackend::RingState {
    io_uring ring;
    bool     ring_initialized   = false;
    bool     buffers_registered = false;
    bool     files_registered   = false;
};

// =====================================================================
// BufferHandleImpl::~BufferHandleImpl —— 出域定义。
//
// 必须在能看到 IoUringIoBackend 完整类型的 TU 里(否则 friend 调用拒编)。
//
// M1 阶段没有真实 AcquireBuffer 路径,所以 owner_ 永远是 nullptr,
// 析构走 "啥也没拿" 分支直接 return。M2 起 owner_ 可能非空,
// 委托给 ReturnBuffer_Internal 处理。
// =====================================================================
BufferHandleImpl::~BufferHandleImpl() {
    if (owner_ == nullptr) {
        return;        // invalid handle / 没拿过 slot
    }
    owner_->ReturnBuffer_Internal(*this);
}

// =====================================================================
// IoUringIoBackend 实现 —— M1 全部 stub。
// =====================================================================

IoUringIoBackend::~IoUringIoBackend() {
    // M2 起此处委托 Close() 作 RAII 安全网。
    // M1 阶段 opened_ 永远 false / ring_state_ 永远 nullptr,delete nullptr 安全。
    delete ring_state_;
    ring_state_ = nullptr;
}

int32_t IoUringIoBackend::Open(const std::string& devicePath,
                                std::uint32_t bufferPoolCount) {
    (void)devicePath;
    (void)bufferPoolCount;
    // M1 stub:暂不实现真实 Open,返回 IO_BACKEND_NOT_OPEN。
    // 这意味着同 build 下 io_backend_contract_test 的 Engine 集成用例会 fail ——
    // 这是 M1 期预期行为,M2 落地后转绿。M1 验收用 io_uring_skeleton_test。
    //
    // M2 起此处会做:
    //   1. open(devicePath, O_DIRECT|O_SYNC|O_RDWR) → fd_ + S_ISBLK / BLKGETSIZE64 校验
    //   2. mmap(count × 1 MiB) → poolBase_,切片填 freeStack_
    //   3. ring_state_ = new RingState; io_uring_queue_init(sq_depth, &ring_state_->ring, 0)
    //   4. (M4) io_uring_register_buffers(...) → buffers_registered_=true
    //   5. (M5) io_uring_register_files(...) → files_registered_=true
    //   6. opened_ = true
    return IO_BACKEND_NOT_OPEN;
}

int32_t IoUringIoBackend::Close() {
    // M1 stub:与 sync 后端对齐 —— 未 Open 状态下 Close 幂等 SUCCESS,
    // 不进入 terminal,is_closed() 仍为 false,允许 M2 起首次 Open 成功。
    //
    // M2 起此处会做:
    //   1. closed_.store(true) —— 拒绝新提交
    //   2. drain:wait_cqe 直到 in_flight_count_ == 0
    //   3. Q7 outstanding_count_ 检查(Debug abort / Release warn)
    //   4. (M5) io_uring_unregister_files
    //   5. (M4) io_uring_unregister_buffers
    //   6. io_uring_queue_exit(&ring_state_->ring); delete ring_state_; ring_state_ = nullptr
    //   7. munmap(poolBase_, poolTotalSize_); close(fd_)
    //   8. opened_ = false
    return SUCCESS;
}

bool IoUringIoBackend::IsOpen() const noexcept {
    return opened_;
}

std::uint64_t IoUringIoBackend::BlockCount() const noexcept {
    return blockCount_;
}

bool IoUringIoBackend::is_closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

BufferHandle IoUringIoBackend::AcquireBuffer() {
    // M1 stub:无 pool 状态可发,返回 invalid handle(等同 Q3 池耗尽契约)。
    //
    // M2 起此处会做:
    //   1. lock(poolMutex_)
    //   2. 若 freeStack_ 空 → 返回 invalid handle(Q3,不报错)
    //   3. 弹一个 slot index → 构造 BufferHandleImpl(填 ptr_/size_/slot_index_/
    //      fixed_buf_index_/owner_)
    //   4. outstanding_count_.fetch_add(1)
    //   5. 返回 BufferHandle{std::move(impl)}
    return BufferHandle{};
}

int32_t IoUringIoBackend::WriteBlock(BlockId, const BufferHandle&) {
    // M1 stub。M3 起真实实现:
    //   1. 检查 closed_ / handle.valid / blockId 范围
    //   2. lock(io_mutex_); in_flight_count_++
    //   3. sqe = io_uring_get_sqe(&ring_state_->ring)
    //   4. M3:io_uring_prep_write(sqe, fd_, ptr, size, offset);
    //      M4:io_uring_prep_write_fixed(sqe, fd_idx, ptr, size, offset, fixed_buf_idx)
    //      M5:sqe->flags |= IOSQE_FIXED_FILE; sqe->fd = 0
    //   5. io_uring_submit_and_wait(&ring_state_->ring, 1)
    //   6. cqe = io_uring_peek_cqe(...); 读 cqe->res; io_uring_cqe_seen(...)
    //   7. in_flight_count_--; 错误映射(SUBMIT_FAILED / IO_FAILED / 短读短写)
    return IO_BACKEND_NOT_OPEN;
}

int32_t IoUringIoBackend::ReadBlock(BlockId, BufferHandle&) {
    // M1 stub。M3 起真实实现(对称 WriteBlock,prep_read / prep_read_fixed)。
    return IO_BACKEND_NOT_OPEN;
}

void IoUringIoBackend::ReturnBuffer_Internal(BufferHandleImpl& impl) noexcept {
    // M1 stub:理论上不会被调到 —— AcquireBuffer 返回的都是 invalid handle,
    // 析构走 "owner_ == nullptr" 早退分支。
    //
    // M2 起此处会做:
    //   1. 若 is_closed() == true → no-op 退出(Q7,backend pool 已释放)
    //   2. lock(poolMutex_); freeStack_.push_back(impl.slot_index_)
    //   3. outstanding_count_.fetch_sub(1)
    (void)impl;
}

} // namespace cabe::io
