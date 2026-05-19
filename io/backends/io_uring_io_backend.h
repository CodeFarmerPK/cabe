/*
 * Project: Cabe
 * Created Time: 2026-04-28
 * Created by: CodeFarmerPK
 *
 * IoUringIoBackend —— P4 io_uring 后端声明(P4 ✅ 全部完成,2026-05-14 收尾)。
 *
 * 进度(详见 doc/p4_io_uring_design.md §13;Engine 公开 API 自 M6 起多了
 * Options.io_uring_sq_depth 一个可选字段,默认 64 完全向下兼容):
 *   M1 ✅ 骨架 + liburing 接入 + TSAN/io_uring 双层阻断
 *   M2 ✅ Open/Close + buffer pool + ring 真实实现(状态机 + Q7 行为对齐 sync)
 *   M3 ✅ 朴素 WriteBlock/ReadBlock(Model A,prep_write/prep_read,裸 fd)
 *   M4 ✅ io_uring_register_buffers + WRITE_FIXED/READ_FIXED + fixed_buf_index_
 *         真用(bench cpu_time 加速 16–82%,远超 5% 验收门)
 *   M5 ✅ io_uring_register_files + IOSQE_FIXED_FILE(register fd 一次,
 *         WriteBlock/ReadBlock 走 fd_idx=0,跳过 fdget/fdput)
 *   M6 ✅ Options.io_uring_sq_depth(D7)+ R12 校验 + 4 个 specific test +
 *         README "Production deployment notes"(R7)+ CABE_HAVE_* feature
 *         gate(D10)
 *   M7 ✅ 内部 batch API(WriteBlocks/ReadBlocks)+ Engine 多 chunk 路径接入(D18):
 *         一次 submit_and_wait(N)+ io_uring_for_each_cqe + cq_advance(N),
 *         省 (N-1) 次 syscall 来回;sync 后端同名接口走 for-loop fallback
 *   M8 ✅ (评估 = 不做)Model A → Model B 升级评估闭环。Model B 在 P6 reactor
 *         阶段被 per-thread ring 架构替代(§5.2 ring 拓扑演进表),不是 P6 的
 *         中间形态;1 MiB 块 + 单 NVMe 下 Model A 单线程已逼近带宽极限;
 *         公开 API 仍是 sync(D3)→ Model B 拿不到真正 overlap 收益。
 *         详见设计稿 §13 M8 决策结论
 *   M9 ✅ bench 总结(bench/baselines/p4-summary.md)+ README/roadmap/设计稿
 *         状态 → "已实施";§20 Open Questions 全部收口
 *
 * 当前实现形态(P4 ✅ 全部完成):
 *   - 设备:O_DIRECT | O_SYNC | O_RDWR 打开裸块设备(同 sync 后端);fd 已
 *           registered(io_uring_register_files),SQE 用 IOSQE_FIXED_FILE +
 *           fd_idx=0 提交,跳过 fdget/fdput
 *   - Buffer pool:mmap(MAP_ANONYMOUS) 一大块,LIFO slot;Open 内整片
 *                 register 为 io_uring fixed buffer(n × 1 MiB iovec,
 *                 fixed_buf_index_ == slot_index_)
 *   - Ring:io_uring_queue_init(sqDepth, ..., 0);M6 起 sqDepth 由
 *           Options.io_uring_sq_depth 透传(默认 64,Open 校验 sq>=pool 且 2 幂)
 *   - 提交模型:Model A(粗 io_mutex_ + io_uring_submit_and_wait(1) +
 *               EAGAIN 一次退避),M7/M8 视情况升级
 *
 * 线程安全(P4 范围):
 *   - Open / Close 由调用方(Engine)串行
 *   - AcquireBuffer / handle 析构归还 由 backend 内部 poolMutex_ 保护
 *   - WriteBlock / ReadBlock 多线程并发提交时由 io_mutex_ 串行(Model A)
 *
 * Q7 配套(沿用 P3 sync 后端约定):
 *   - 维护 outstanding_count_ 原子计数(BufferHandle 数)
 *   - 维护 in_flight_count_ 原子计数(P4 新增,跟踪未消费的 CQE)
 *   - Close drain 等 in_flight_count_ → 0 才反注册并 queue_exit
 *   - Close 检查 outstanding_count_:Debug 不为 0 abort,Release 警告 + 强制释放
 *   - is_closed() 给 BufferHandleImpl::~ 在 Close 后 no-op 退出用
 *
 * PIMPL 隐藏 liburing:
 *   - 本头不 #include <liburing.h>,避免 Engine 等下游 TU 强依赖 liburing 头
 *   - io_uring 真实状态(io_uring 结构体、register 标志位)放在内部 struct
 *     RingState,只在 io_uring_io_backend.cpp 里通过 #include <liburing.h> 暴露
 *
 * 详细设计:doc/p4_io_uring_design.md
 */

#ifndef CABE_IO_BACKENDS_IO_URING_IO_BACKEND_H
#define CABE_IO_BACKENDS_IO_URING_IO_BACKEND_H

#include "common/error_code.h"
#include "common/structs.h"
#include "io/buffer_handle.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace cabe::io {

class BufferHandleImpl;     // 完整定义见 io_uring_buffer_handle_impl.h

class IoUringIoBackend {
public:
    IoUringIoBackend()  = default;
    ~IoUringIoBackend();

    IoUringIoBackend(const IoUringIoBackend&)            = delete;
    IoUringIoBackend& operator=(const IoUringIoBackend&) = delete;
    IoUringIoBackend(IoUringIoBackend&&)                 = delete;
    IoUringIoBackend& operator=(IoUringIoBackend&&)      = delete;

    // ===== 生命周期(状态机为终态 Close,与 sync 后端语义对齐)=====
    //   (opened=false,closed=false) ──Open──▶ (opened=true,closed=false)
    //                                              │
    //                                            Close
    //                                              ▼
    //                              (opened=false,closed=true)   ← terminal
    //
    // M1 stub:Open 返回 IO_BACKEND_NOT_OPEN(暂未实现真实路径);
    //          Close 在未 Open 上幂等 SUCCESS,不进入 terminal,
    //          允许 M2 起在同实例上首次 Open 成功。
    // 第 3 个参数 sqDepth(P4 M6 / D7):io_uring SQ depth。
    //   - 必须是 2 的幂(io_uring_queue_init 老内核硬性要求,统一锁紧)
    //   - 必须 >= bufferPoolCount(R12,M7 batch 上限保护)
    //   - 校验失败返回 POOL_INVALID_PARAMS(与 bufferPoolCount==0 同分类)
    // default = 64 让旧 2-arg 调用点(contract test 等)不需要逐一改动;
    // sync 后端有同形签名(但忽略此值),IoBackendTraits 共契约。
    int32_t       Open(const std::string& devicePath,
                       std::uint32_t      bufferPoolCount,
                       std::uint32_t      sqDepth = 64);
    int32_t       Close();
    bool          IsOpen()      const noexcept;
    std::uint64_t BlockCount()  const noexcept;
    // P4.5 M4:设备 fd(供 FreeList TRIM / Engine 探测 discard 支持)。
    // 未 Open 时 fd_ == -1,调用方据此跳过 TRIM。
    [[nodiscard]] int GetDeviceFd() const noexcept { return fd_; }

    // ===== Buffer 生命周期 =====
    // M1 stub:返回 invalid handle(契约上同 Q3 池耗尽路径)。
    // M2 起从 freeStack_ 弹 slot,构造 BufferHandleImpl(填 fixed_buf_index_)。
    BufferHandle AcquireBuffer();

    // ===== 块 I/O(阻塞到完成,线程安全)=====
    // M1 stub:返回 IO_BACKEND_NOT_OPEN
    // M3 起真实实现(Model A);M4 起改用 *_FIXED;M5 起加 IOSQE_FIXED_FILE
    int32_t WriteBlock(BlockId blockId, const BufferHandle& handle);
    int32_t ReadBlock (BlockId blockId, BufferHandle&       handle);

    // ===== 批量块 I/O(P4 M7,IoBackendTraits 一致)=====
    // 真实批量提交:一次锁内 prep N 个 SQE → submit_and_wait(N)
    // → io_uring_for_each_cqe 一次性 sweep → cq_advance(N),省 (N-1) 次
    // syscall 来回。N = batch.size(),受调用方限制不超过 sq_depth(R12 在
    // Open 时校验过 sq>=pool,Engine 多 chunk 路径按 pool 分批,batch.size()
    // <= pool_count <= sq_depth)。
    //
    // 错误语义:任一 CQE.res < 0 视为整批失败,返回首个非 SUCCESS 错码
    //   (与 N 次单笔 WriteBlock 行为一致);已完成的 I/O 不回滚,由
    //   Engine 在写齐 metaIndex 之前不暴露给读路径来避免半成品可见。
    // EAGAIN 处理:批量场景里 submit 失败一次性返回 IO_BACKEND_QUEUE_FULL,
    //   不做单笔退避(避免 (N - submitted) 笔的复杂回滚)。Engine 的多
    //   chunk 路径按 pool 大小分批已经把 N <= pool <= sq_depth 锁紧。
    // batch.size() == 0 直接返回 SUCCESS(与 sync 后端一致)。
    int32_t WriteBlocks(std::span<const std::pair<BlockId, const BufferHandle*>> batch);
    int32_t ReadBlocks (std::span<const std::pair<BlockId, BufferHandle*>>       batch);

    // ===== Q7 支持 =====
    bool is_closed() const noexcept;

private:
    // BufferHandleImpl 析构调它把自己归还到池。Impl 是 friend,
    // ReturnBuffer_Internal 保持私有,不暴露给 Engine。
    friend class BufferHandleImpl;
    void ReturnBuffer_Internal(BufferHandleImpl& impl) noexcept;

    // ----------------------------------------------------------------
    // 生命周期状态(M1 stub 阶段全部默认值)
    // ----------------------------------------------------------------
    bool                       opened_            = false;

    // ----------------------------------------------------------------
    // 设备字段(M2 起填实)
    // ----------------------------------------------------------------
    int                        fd_                = -1;
    std::string                devicePath_;
    std::uint64_t              blockCount_        = 0;

    // ----------------------------------------------------------------
    // io_uring 状态:PIMPL 包裹,定义在 .cpp 里,本头不暴露 liburing.h
    //   ring_state_       : 指向内部 RingState(包含 io_uring 结构体 + 注册状态)
    //                       M1 stub 阶段保持 nullptr;M2 起在 Open 内 new
    //   io_mutex_         : Model A 多线程提交串行化(M3 起生效)
    //   in_flight_count_  : 已 submit 但未 cqe_seen 的 op 数;Close drain 用(M2 起)
    // ----------------------------------------------------------------
    struct RingState;
    RingState*                 ring_state_        = nullptr;
    mutable std::mutex         io_mutex_;
    std::atomic<std::uint32_t> in_flight_count_   {0};

    // ----------------------------------------------------------------
    // Q7 多线程跟踪(同 sync 后端语义)
    //   closed_            : Close 真正生效后置 true,永不复位
    //   outstanding_count_ : 已 Acquire 但未 dtor 的 BufferHandle 数
    // ----------------------------------------------------------------
    std::atomic<bool>          closed_            {false};
    std::atomic<std::uint32_t> outstanding_count_ {0};

    // ----------------------------------------------------------------
    // Pool 状态(M2 起 mmap + 切片;M4 起 io_uring_register_buffers)
    //   poolBase_         : mmap 基地址,n × 1 MiB 切片
    //   poolBufferCount_  : slot 总数(== Open 入参 bufferPoolCount)
    //   poolTotalSize_    : poolBufferCount_ * CABE_VALUE_DATA_SIZE
    //   freeStack_        : LIFO 空闲 slot 索引
    // ----------------------------------------------------------------
    mutable std::mutex         poolMutex_;
    char*                      poolBase_          = nullptr;
    std::uint32_t              poolBufferCount_   = 0;
    std::size_t                poolTotalSize_     = 0;
    std::vector<std::uint32_t> freeStack_;
};

} // namespace cabe::io

#endif // CABE_IO_BACKENDS_IO_URING_IO_BACKEND_H
