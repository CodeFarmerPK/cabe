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
 * 选择方式(P3 M4 起 CMake 缓存变量 CABE_IO_BACKEND 控制):
 *   cmake -DCABE_IO_BACKEND=sync       → CABE_IO_BACKEND_SYNC=1     (P3 默认)
 *   cmake -DCABE_IO_BACKEND=io_uring   → CABE_IO_BACKEND_IO_URING=1 (P4 起)
 *   cmake -DCABE_IO_BACKEND=spdk       → CABE_IO_BACKEND_SPDK=1     (P9 起)
 * 详见 CMakeLists.txt 的 CABE_IO_BACKEND 缓存变量分支;脚本入口在
 *   scripts/run-tests.sh --backend=BACKEND
 *   scripts/run-bench.sh --backend=BACKEND
 * 也可用 CMakePresets.json 里的 sync-* / io_uring-* 预设(IDE 友好)。
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
#include <span>
#include <string>
#include <type_traits>
#include <utility>

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
  #error "No IO backend selected; pass -DCABE_IO_BACKEND=sync (or io_uring / spdk) to cmake"
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
    std::uint32_t           sqDepth,   // M6:io_uring SQ depth(sync 后端忽略)
    BlockId                 blockId,
    BufferHandle&           handle,
    const BufferHandle&     chandle,
    // M7:批量 I/O 形参(span 视图,owner 在调用方)
    std::span<const std::pair<BlockId, const BufferHandle*>> writeBatch,
    std::span<const std::pair<BlockId, BufferHandle*>>       readBatch
) {
    // 生命周期
//
    // Open 的第 3 个参数 sqDepth(P4 M6 起加):
    //   - sync 后端:忽略此值,签名一致只为 trait 一致
    //   - io_uring 后端:用作 io_uring_queue_init 的 entries 参数;
    //     校验 sq_depth >= pool_count(R12)且是 2 的幂(D7)
    //
    // 各 backend 的 Open 提供 default 参数(== 64),保证 2-arg 调用
    // (旧 contract test / 旧测试 fixture)仍合法,无需逐一改动。
    { t.Open(path, poolCount, sqDepth) } -> std::same_as<std::int32_t>;
    { t.Close() }                        -> std::same_as<std::int32_t>;
    { ct.IsOpen() }                      -> std::same_as<bool>;
    { ct.BlockCount() }                  -> std::same_as<std::uint64_t>;
    { ct.BlockCount() }                  -> std::same_as<std::uint64_t>;

    // P4.5 M4:暴露设备 fd。FreeList 用它发 ioctl(BLKDISCARD)(TRIM),
    // Engine.Open 用它读 sysfs 探测 discard 支持。未 Open 时返回 -1,
    // 调用方据此跳过 TRIM。两后端 inline 返回内部 fd_。
    { ct.GetDeviceFd() }                 -> std::same_as<int>;

    // Buffer 生命周期(归还由 BufferHandle 析构 → BufferHandleImpl::~ 完成)
    { t.AcquireBuffer() }              -> std::same_as<BufferHandle>;

    // 块 I/O(阻塞到完成)
    { t.WriteBlock(blockId, chandle) } -> std::same_as<std::int32_t>;
    { t.ReadBlock(blockId, handle) }   -> std::same_as<std::int32_t>;

    // 批量块 I/O(P4 M7):一次提交 N 个 chunk 的请求,等齐完成。
    //   - sync 后端:for-loop 调用 WriteBlock/ReadBlock,首个错误即返回,
    //     失败前已成功的项不回滚(语义与单笔 WriteBlock 调用 N 次完全等价)
    //   - io_uring 后端:io_uring_prep_*_fixed N 个 SQE → submit_and_wait(N)
    //     → io_uring_for_each_cqe + cq_advance(N),省 (N-1) 次 syscall 来回;
    //     任一 CQE.res < 0 即视为整批失败,返回首个非 SUCCESS 错码
    //
    // 调用方约束(Engine 多 chunk 路径已保证):
    //   - batch.size() > 0
    //   - 每个 BufferHandle* 非空且仍持有有效 buffer
    //   - batch.size() <= sq_depth(io_uring 后端,否则 submit 失败)
    //   - 同一 batch 内 BlockId 不重复(Engine 通过 AllocateChunkIds 自然保证)
    //
    // 不保证原子性:io_uring 批量提交是 N 个独立 I/O,失败时已写部分留在设备上,
    // 由 Engine 的 chunkIndex/metaIndex 操作时机(写齐再更新)避免对外可见。
    { t.WriteBlocks(writeBatch) } -> std::same_as<std::int32_t>;
    { t.ReadBlocks(readBatch) }   -> std::same_as<std::int32_t>;
};

static_assert(IoBackendTraits<IoBackend>,
              "Selected IoBackend must satisfy IoBackendTraits");

static_assert(!std::is_copy_constructible_v<IoBackend>,
              "IoBackend must be non-copyable (持 fd / ring / bdev 等独占资源)");
static_assert(!std::is_move_constructible_v<IoBackend>,
              "IoBackend must be non-movable (Engine 把它做值成员,move 会失稳)");

} // namespace cabe::io

#endif // CABE_IO_IO_BACKEND_H
