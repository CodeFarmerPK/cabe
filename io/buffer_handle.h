/*
 * Project: Cabe
 * Created Time: 2026-04-27
 * Created by: CodeFarmerPK
 *
 * BufferHandle —— IoBackend 发放的不透明 I/O 缓冲句柄(PIMPL 风格)。
 *
 * 设计契约(P3 M1 起):
 *   1) 由 IoBackend::AcquireBuffer 发放。默认构造产生 invalid handle
 *      (.valid() == false),IoBackend 内部用作"池耗尽"信号(Q3)。
 *   2) 析构自动归还到 backend pool(RAII,Q1 决定)。归还失败的处理由各
 *      backend 在 BufferHandleImpl 析构里实现(Q7:Close 后 no-op)。
 *   3) Move-only:转移所有权,源 handle 变 invalid;copy 已禁用。
 *   4) 内容契约:刚发放的 buffer 内容**未定义**(Q2 决定)。调用方必须在
 *      使用前覆盖,否则磁盘上会留旧数据;Engine::Put 小 value 分支必须
 *      负责 memset 尾部到 CABE_VALUE_DATA_SIZE 的边界。
 *
 * PIMPL:
 *   - 公开头(本文件)只对 BufferHandleImpl 做前向声明。
 *   - 各 backend 在私有头(io/backends/<backend>_buffer_handle_impl.h)里
 *     给出 BufferHandleImpl 的完整定义。同一编译单元(由
 *     CABE_IO_BACKEND_* 宏激活)只可能看到一种 Impl 形态。
 *   - 析构 / move 的实现必须放在 io/buffer_handle.cpp(unique_ptr<Impl>
 *     需要看到 sizeof(Impl) 才能实例化删除器)。
 *
 * 字段契约(BufferHandleImpl 必须满足,buffer_handle.cpp 的访问器依赖):
 *   - ptr_  : char*       指向 buffer 起始
 *   - size_ : std::size_t buffer 字节数
 *   各 backend 可附加自己的字段(slot_index_ / registered_index_ / spdk_buf 等)。
 *
 * 线程安全:
 *   - 同一 handle 不能跨线程并发访问(单一所有权);
 *   - 不同 handle 的并发操作安全(归还到 pool 的并发由 backend 内部锁保护)。
 */

#ifndef CABE_IO_BUFFER_HANDLE_H
#define CABE_IO_BUFFER_HANDLE_H

#include <cstddef>
#include <memory>

namespace cabe::io {

// 前向声明:Impl 在选定 backend 的私有头里完整定义
class BufferHandleImpl;

class BufferHandle {
public:
    BufferHandle() noexcept;                            // invalid
    ~BufferHandle();                                    // RAII: 若 valid 则归还

    BufferHandle(BufferHandle&&) noexcept;
    BufferHandle& operator=(BufferHandle&&) noexcept;

    BufferHandle(const BufferHandle&)            = delete;
    BufferHandle& operator=(const BufferHandle&) = delete;

    // 状态查询
    [[nodiscard]] bool        valid() const noexcept;
    [[nodiscard]] std::size_t size()  const noexcept;

    // 数据访问。invalid handle 上调用返回 nullptr(由调用方先查 valid())。
    [[nodiscard]] char*       data()       noexcept;
    [[nodiscard]] const char* data() const noexcept;

private:
    // 各 backend 类通过 friend 调内部构造函数装入自己的 Impl。
    // 一个 TU 里只激活一个 backend,所以最多生效一条 friend。
#if defined(CABE_IO_BACKEND_SYNC)
    friend class SyncIoBackend;
#elif defined(CABE_IO_BACKEND_IO_URING)
    friend class IoUringIoBackend;     // P4 接入
#elif defined(CABE_IO_BACKEND_SPDK)
    friend class SpdkIoBackend;        // P9 接入
#endif

    // 仅 backend 内部使用的构造:接管已构造好的 Impl
    explicit BufferHandle(std::unique_ptr<BufferHandleImpl> impl) noexcept;

    std::unique_ptr<BufferHandleImpl> impl_;
};

} // namespace cabe::io

#endif // CABE_IO_BUFFER_HANDLE_H
