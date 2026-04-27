/*
 * Project: Cabe
 * Created Time: 2026-04-27
 * Created by: CodeFarmerPK
 *
 * IoBackend —— 编译期 dispatch 入口。
 *
 * Engine 通过 cabe::io::IoBackend(类型别名)操作 I/O,具体类型由 CMake
 * 在编译期通过 CABE_IO_BACKEND_* 宏选定。无虚函数、无 unique_ptr、无
 * runtime if —— 全部 inline 友好。
 *
 * 选择方式(P3 M4 阶段会包成 CMake 缓存变量 CABE_IO_BACKEND):
 *   cmake -DCABE_IO_BACKEND=sync       → CABE_IO_BACKEND_SYNC=1
 *   cmake -DCABE_IO_BACKEND=io_uring   → CABE_IO_BACKEND_IO_URING=1
 *   cmake -DCABE_IO_BACKEND=spdk       → CABE_IO_BACKEND_SPDK=1
 *
 * 强制接口契约:IoBackendTraits concept + static_assert 编译期校验。
 *   - 任何 backend 漏方法、签名错都立刻编译失败,定位到此处
 *   - 同时校验 IoBackend 不可 copy / move(避免 fd / ring / bdev 双重持有)
 */

#ifndef CABE_IO_IO_BACKEND_H
#define CABE_IO_IO_BACKEND_H

#include "common/structs.h"
#include "io/buffer_handle.h"

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>

#if defined(CABE_IO_BACKEND_SYNC)
  #include "io/backends/sync_io_backend.h"
  namespace cabe::io { using IoBackend = SyncIoBackend; }
#elif defined(CABE_IO_BACKEND_IO_URING)
  #include "io/backends/io_uring_io_backend.h"        // P4 接入
  namespace cabe::io { using IoBackend = IoUringIoBackend; }
#elif defined(CABE_IO_BACKEND_SPDK)
  #include "io/backends/spdk_io_backend.h"            // P9 接入
  namespace cabe::io { using IoBackend = SpdkIoBackend; }
#else
  #error "No IO backend selected; CMake must pass -DCABE_IO_BACKEND_SYNC=1 (or _IO_URING / _SPDK)"
#endif

namespace cabe::io {

// ---------------------------------------------------------------------
// IoBackend 必须满足的接口契约(concept 强校验)。
// 任何 backend 漏方法或签名不一致 → static_assert 报错,定位到此处。
// ---------------------------------------------------------------------
template <typename T>
concept IoBackendTraits = requires(
    T                       t,
    const T                 ct,
    const std::string&      path,
    std::uint32_t           poolCount,
    BlockId                 blockId,
    BufferHandle&           handle,
    const BufferHandle&     chandle
) {
    // 生命周期
    { t.Open(path, poolCount) }        -> std::same_as<std::int32_t>;
    { t.Close() }                      -> std::same_as<std::int32_t>;
    { ct.IsOpen() }                    -> std::same_as<bool>;
    { ct.BlockCount() }                -> std::same_as<std::uint64_t>;

    // Buffer 生命周期(归还由 BufferHandle 析构 → BufferHandleImpl::~ 完成)
    { t.AcquireBuffer() }              -> std::same_as<BufferHandle>;

    // 块 I/O(阻塞到完成)
    { t.WriteBlock(blockId, chandle) } -> std::same_as<std::int32_t>;
    { t.ReadBlock(blockId, handle) }   -> std::same_as<std::int32_t>;
};

static_assert(IoBackendTraits<IoBackend>,
              "Selected IoBackend must satisfy IoBackendTraits");

static_assert(!std::is_copy_constructible_v<IoBackend>,
              "IoBackend must be non-copyable (持 fd / ring / bdev 等独占资源)");
static_assert(!std::is_move_constructible_v<IoBackend>,
              "IoBackend must be non-movable (Engine 把它做值成员,move 会失稳)");

} // namespace cabe::io

#endif // CABE_IO_IO_BACKEND_H
