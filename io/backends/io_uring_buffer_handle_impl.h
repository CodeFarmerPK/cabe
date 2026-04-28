/*
 * Project: Cabe
 * Created Time: 2026-04-28
 * Created by: CodeFarmerPK
 *
 * BufferHandleImpl(io_uring 后端版本)—— PIMPL 内部数据。
 *
 * 字段契约(io/buffer_handle.cpp 的访问器依赖,所有 backend 必须满足):
 *   - ptr_  : char*       指向 1 MiB pool slot 起始(必须存在;invalid 时为 nullptr)
 *   - size_ : std::size_t buffer 字节数(通常 == CABE_VALUE_DATA_SIZE)
 *
 * 字段(io_uring 后端专属):
 *   - slot_index_       : pool 内的 slot 索引,归还时 backend 用它推回 freeStack_
 *   - fixed_buf_index_  : 注册到 io_uring 的 fixed buffer 索引(M4 起填值)
 *                          M1 占位为 0;M4 起 == slot_index_(n × 1 MiB iovec 一一对应)
 *   - owner_            : 反向指针,析构时调 owner_->ReturnBuffer_Internal
 *
 * 与 sync 版本的差异:
 *   - 多一个 fixed_buf_index_ 字段(M4 起用于 IORING_OP_*_FIXED 的 buf_index 参数)
 *   - owner_ 类型从 SyncIoBackend* 变成 IoUringIoBackend*
 *
 * 析构语义(定义在 io_uring_io_backend.cpp,需要 IoUringIoBackend 完整类型):
 *   - owner_ == nullptr           → invalid handle,啥也没拿,直接 return
 *   - owner_->is_closed() == true → Q7 路径(Close 已发生),静默放弃归还
 *   - 其他                         → 正常归还到池
 *
 * 详细设计:doc/p4_io_uring_design.md §6.2、§9
 */

#ifndef CABE_IO_BACKENDS_IO_URING_BUFFER_HANDLE_IMPL_H
#define CABE_IO_BACKENDS_IO_URING_BUFFER_HANDLE_IMPL_H

#include <cstddef>
#include <cstdint>

namespace cabe::io {

class IoUringIoBackend;     // 前向声明,避免与 io_uring_io_backend.h 形成循环依赖

class BufferHandleImpl {
public:
    BufferHandleImpl()  = default;
    ~BufferHandleImpl();    // 出域定义在 io_uring_io_backend.cpp

    BufferHandleImpl(const BufferHandleImpl&)            = delete;
    BufferHandleImpl& operator=(const BufferHandleImpl&) = delete;

    // 字段直接 public:不是 API 边界,只在以下两处访问:
    //   - io/buffer_handle.cpp 的 data()/size()/valid() 读 ptr_ / size_
    //   - io_uring_io_backend.cpp 的 ReturnBuffer_Internal 读 slot_index_
    //   - io_uring_io_backend.cpp 的 WriteBlock/ReadBlock 读 fixed_buf_index_(M4 起)
    char*               ptr_              = nullptr;
    std::size_t         size_             = 0;
    std::uint32_t       slot_index_       = 0;
    std::uint32_t       fixed_buf_index_  = 0;     // M4 起填值;M1/M2/M3 占位
    IoUringIoBackend*   owner_            = nullptr;
};

} // namespace cabe::io

#endif // CABE_IO_BACKENDS_IO_URING_BUFFER_HANDLE_IMPL_H
