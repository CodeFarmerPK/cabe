/*
 * Project: Cabe
 * Created Time: 2026-04-27
 * Created by: CodeFarmerPK
 *
 * BufferHandle 的 PIMPL 出口。所有需要看到 sizeof(BufferHandleImpl) 的
 * 成员函数都定义在此处:析构、move、data() / size() / valid()。
 *
 * 编译期校验:CABE_IO_BACKEND_* 宏必须由 CMake 提供;否则 #error。
 * 见 CMakeLists.txt 的 target_compile_definitions(cabe_lib PUBLIC ...)。
 */

#include "io/buffer_handle.h"

// 必须 include 完整的 BufferHandleImpl 定义 —— unique_ptr<Impl> 析构需要它
#if defined(CABE_IO_BACKEND_SYNC)
  #include "io/backends/sync_buffer_handle_impl.h"
#elif defined(CABE_IO_BACKEND_IO_URING)
  #include "io/backends/io_uring_buffer_handle_impl.h"   // P4 接入
#elif defined(CABE_IO_BACKEND_SPDK)
  #include "io/backends/spdk_buffer_handle_impl.h"       // P9 接入
#else
  #error "No CABE_IO_BACKEND_* macro defined; CMake must pass -DCABE_IO_BACKEND_SYNC=1 (or _IO_URING / _SPDK)"
#endif

namespace cabe::io {

// =====================================================================
// 五法则:默认构造、析构、move 构造、move 赋值、私有 Impl 接管构造。
// 析构与 move 构造用 = default —— unique_ptr 的语义在 Impl 完整可见后正确生成。
// =====================================================================

BufferHandle::BufferHandle() noexcept = default;

BufferHandle::~BufferHandle() = default;
// 归还逻辑封装在 BufferHandleImpl::~BufferHandleImpl 里:
//   - 不需要显式 backend 调用 —— Impl 自己持 owner_ 指针,析构时调
//     owner_->ReturnBuffer_Internal(*this)。
//   - Q7:Impl 析构内会先查 owner_->is_closed(),已 closed 则 no-op,
//     避免 use-after-free。

BufferHandle::BufferHandle(BufferHandle&&) noexcept = default;

BufferHandle& BufferHandle::operator=(BufferHandle&& other) noexcept {
    if (this == &other) return *this;
    // 显式 reset:让自己持有的 Impl(若 valid)先走完归还路径,再接管 other。
    // 直接 = std::move(other.impl_) 也会触发旧 Impl 析构,但显式写出更清晰。
    impl_.reset();
    impl_ = std::move(other.impl_);
    return *this;
}

BufferHandle::BufferHandle(std::unique_ptr<BufferHandleImpl> impl) noexcept
    : impl_(std::move(impl)) {}

// =====================================================================
// 访问器:依赖 BufferHandleImpl 的 ptr_ / size_ 字段(各 backend 共有契约)。
// =====================================================================

bool BufferHandle::valid() const noexcept {
    return impl_ != nullptr && impl_->ptr_ != nullptr;
}

std::size_t BufferHandle::size() const noexcept {
    return impl_ ? impl_->size_ : 0;
}

char* BufferHandle::data() noexcept {
    return impl_ ? impl_->ptr_ : nullptr;
}

const char* BufferHandle::data() const noexcept {
    return impl_ ? impl_->ptr_ : nullptr;
}

} // namespace cabe::io
