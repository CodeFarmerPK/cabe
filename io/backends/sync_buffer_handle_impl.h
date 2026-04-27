/*
* Project: Cabe
 * Created Time: 2026-04-27
 * Created by: CodeFarmerPK
 *
 * BufferHandleImpl(sync 后端版本)—— PIMPL 内部数据。
 *
 * 字段契约(io/buffer_handle.cpp 的访问器依赖,所有 backend 必须满足):
 *   - ptr_  : char*       指向 1 MiB pool slot 起始(必须存在;invalid 时为 nullptr)
 *   - size_ : std::size_t buffer 字节数(通常 == CABE_VALUE_DATA_SIZE)
 *
 * 字段(sync 后端专属):
 *   - slot_index_ : pool 内的 slot 索引,归还时 backend 用它推回 freeStack_
 *   - owner_      : 反向指针,析构时调 owner_->ReturnBuffer_Internal
 *
 * 析构语义(定义在 sync_io_backend.cpp,因为需要 SyncIoBackend 完整类型):
 *   - owner_ == nullptr           → invalid handle,啥也没拿,直接 return
 *   - owner_->is_closed() == true → Q7 路径(Close 已发生),静默放弃归还
 *   - 其他                         → 正常归还到池
 */

#ifndef CABE_IO_BACKENDS_SYNC_BUFFER_HANDLE_IMPL_H
#define CABE_IO_BACKENDS_SYNC_BUFFER_HANDLE_IMPL_H

#include <cstddef>
#include <cstdint>

namespace cabe::io {

    class SyncIoBackend;     // 前向声明,避免与 sync_io_backend.h 形成循环依赖

    class BufferHandleImpl {
    public:
        BufferHandleImpl()  = default;
        ~BufferHandleImpl();    // 出域定义在 sync_io_backend.cpp

        BufferHandleImpl(const BufferHandleImpl&)            = delete;
        BufferHandleImpl& operator=(const BufferHandleImpl&) = delete;

        // 字段直接 public:不是 API 边界,只在 buffer_handle.cpp 的访问器
        // 与 sync_io_backend.cpp 的 ReturnBuffer_Internal 内访问。
        char*           ptr_        = nullptr;
        std::size_t     size_       = 0;
        std::uint32_t   slot_index_ = 0;
        SyncIoBackend*  owner_      = nullptr;
    };

} // namespace cabe::io

#endif // CABE_IO_BACKENDS_SYNC_BUFFER_HANDLE_IMPL_H
