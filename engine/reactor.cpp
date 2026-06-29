#include "engine/reactor.h"
#include "common/error_code.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"          // P7M2：GetWallTimeNs

#include <cstring>
#include <utility>

namespace cabe {

    namespace {
        // 每调用线程一个唤醒字，跨多个 reactor 共享（为 M4 fan-out 准备）。生命周期 = 线程，
        // 比 per-op 栈节点活得久 → reactor 的 notify 绝不打在已析构的 OpNode 上（避"最后触碰"UB）。
        thread_local std::atomic<std::uint32_t> g_wake_gen{0};
    } // namespace

    Reactor::Reactor(DeviceContext&& dc, const Options& opts)
        : options_(opts), dc_(std::move(dc)) {
        // dc move 进来后，三个组件的 opts_ 还指向 Engine::options_；重指到本 reactor 的副本，
        // 此后 wal_level 改写/现读全在本 reactor 线程、读写本副本（per-reactor，无 race）。
        dc_.io.RebindOptions(&options_);
        dc_.wal.RebindOptions(&options_);
        dc_.snapshot.RebindOptions(&options_);
    }

    Reactor::~Reactor() {
        if (thread_.joinable()) Stop();   // 兜底：正常应已 Stop 过（thread 不再 joinable）
        if (!dc_closed_) CloseDc();        // Start 失败/线程没起的路径：关 dc 防 fd 泄漏
    }

    std::int32_t Reactor::Start() {
        try {
            thread_ = std::thread([this] { Run(); });
        } catch (...) {
            return err::kEngineReactorStartFailed;   // 明显故障（线程/资源创建失败），简单判断，不让异常逃逸
        }
        return err::kSuccess;
    }

    std::int32_t Reactor::Stop() {
        if (!thread_.joinable()) return close_result_;   // 已停过 / 没起过（幂等）
        OpNode stop{};                                   // 栈上；join 即同步，Stop op 不需要 wake
        stop.type = OpType::Stop;
        Submit(&stop);
        thread_.join();
        return close_result_;
    }

    // P7M3（多生产者）：push 必须是 RMW（CAS）。每个 CAS 读到前一 push 的值即并入其 release 序列
    //   （C++20：RMW 不论自身内存序都算入序列），于是 DrainAll 的单次 acquire-exchange 能看到链上
    //   **所有**节点的内容（各 push 在 CAS 前对 op 字段的普通写，经 release 序列对单消费者可见）。
    //   改成非 RMW 的 store-push 会断链 → 多生产者下消费者看不到部分节点。勿动。
    void Reactor::Submit(OpNode* op) noexcept {
        OpNode* old = inbox_.load(std::memory_order_relaxed);
        do {
            op->next = old;
        } while (!inbox_.compare_exchange_weak(old, op,
                     std::memory_order_release, std::memory_order_relaxed));   // release：发布 op 内容 + 链入序列
        inbox_.notify_one();   // 唤醒 reactor（栈顶唯一等待者）
    }

    OpNode* Reactor::DrainAll() noexcept {
        // P7M3：acquire 不可降为 relaxed——它 synchronizes-with 链尾 push 的 release（经 release 序列
        //   含全部 push），保证本消费者看到所有被推 op 的内容。这是多生产者可见性的根因（见 Submit）。
        return inbox_.exchange(nullptr, std::memory_order_acquire);
    }

    OpNode* Reactor::Reverse(OpNode* head) noexcept {
        OpNode* prev = nullptr;
        while (head != nullptr) {
            OpNode* n = head->next;
            head->next = prev;
            prev = head;
            head = n;
        }
        return prev;
    }

    void Reactor::Finalize(OpNode* op, std::int32_t rc) noexcept {
        std::atomic<std::uint32_t>* w = op->wake;        // 先存（store 后 op 可能就没了）
        op->result.store(rc, std::memory_order_release); // 对 op 的最后一次访问
        w->fetch_add(1, std::memory_order_release);
        w->notify_one();                                 // 精确唤醒该调用线程
    }

    void Reactor::FailWakeChain(OpNode* head) noexcept {
        while (head != nullptr) {
            OpNode* n = head->next;                       // 先存 next
            Finalize(head, err::kEngineNotOpen);
            head = n;
        }
    }

    void Reactor::CloseDc() noexcept {
        const std::int32_t src = dc_.snapshot.Close();
        const std::int32_t wrc = dc_.wal.Close();
        const std::int32_t irc = dc_.io.Close();
        if (close_result_ == err::kSuccess) {
            close_result_ = (src != err::kSuccess) ? src : (wrc != err::kSuccess) ? wrc : irc;
        }
        dc_closed_ = true;
    }

    void Reactor::Run() {
        while (true) {
            OpNode* batch = DrainAll();
            if (batch == nullptr) {
                inbox_.wait(nullptr, std::memory_order_acquire);   // 空则阻塞，push 时被唤醒
                continue;
            }
            for (OpNode* op = Reverse(batch); op != nullptr; ) {
                OpNode* next = op->next;                  // 先存 next（Finalize 后 op 可能就没了）
                if (op->type == OpType::Stop) {
                    // drain-then-close：Stop 之前的 op 已处理完；关 dc，唤醒掉队 op，退出。
                    CloseDc();
                    FailWakeChain(next);                  // 本批 Stop 之后（到达序）的 op
                    FailWakeChain(Reverse(DrainAll()));   // 关闭期间新 push 进来的
                    return;                               // 线程结束
                }
                std::int32_t rc;
                switch (op->type) {
                    case OpType::Get:         rc = ExecuteGet(op);         break;
                    case OpType::Put:         rc = ExecutePut(op);         break;
                    case OpType::Delete:      rc = ExecuteDelete(op);      break;
                    case OpType::SetWalLevel: rc = ExecuteSetWalLevel(op); break;
                    case OpType::Snapshot:    rc = ExecuteSnapshot(op);    break;
                    default:                  rc = err::kEngineNotImplemented; break;
                }
                Finalize(op, rc);
                op = next;
            }
        }
    }

    std::int32_t Reactor::ExecuteGet(OpNode* op) {
        // 现有 Get 逻辑原样搬进 reactor（改 dc 来源 + 输出到 op->out + 验证已上移调用线程）。
        // M1 测试只走到 Lookup miss 那条；命中路径（Read/CRC/memcpy）随 M2 的 Put-Get 联动测。
        ValueMeta meta{};
        std::int32_t rc = dc_.meta_index.Lookup(op->key, &meta);
        if (rc != err::kSuccess) return rc;              // miss → kIndexKeyNotFound

        std::byte* buf = dc_.pool.Allocate();            // reactor 私有 pool，单线程无锁
        if (buf == nullptr) return err::kEnginePoolExhausted;

        rc = dc_.io.Read(meta.block.block_idx(), buf);
        if (rc != err::kSuccess) {
            dc_.pool.Free(buf);
            return rc;
        }

        const std::uint32_t crc_check = util::CRC32(DataView{buf, kValueSize});
        if (crc_check != meta.crc) {
            CABE_LOG_ERROR("CRC32 不匹配: 存储=0x%08X 读出=0x%08X", meta.crc, crc_check);
            dc_.pool.Free(buf);
            return err::kEngineDataCorrupted;
        }

        std::memcpy(op->out.data(), buf, kValueSize);    // 写到 caller buffer（同步阻塞中，全程有效）
        dc_.pool.Free(buf);
        return err::kSuccess;
    }

    // ---- P7M2：写执行体（逐字照搬 P6M4 Engine::Put/Delete，改 devices_[i]→dc_、入参来自 op）----

    std::int32_t Reactor::ExecutePut(OpNode* op) {
        BlockId block_id{};
        std::int32_t rc = dc_.block_allocator.Acquire(&block_id);
        if (rc != err::kSuccess) return rc;                                  // kEngineNoSpace
        std::byte* buf = dc_.pool.Allocate();
        if (buf == nullptr) {
            dc_.block_allocator.Recycle(block_id);
            return err::kEnginePoolExhausted;
        }
        std::memcpy(buf, op->value.data(), kValueSize);
        const std::uint32_t value_crc = util::CRC32(op->value);
        const std::uint64_t now       = util::GetWallTimeNs();
        rc = dc_.io.Write(block_id.block_idx(), buf);                        // FUA 由 io 读 opts_->wal_level 定
        dc_.pool.Free(buf);
        if (rc != err::kSuccess) {
            dc_.block_allocator.Recycle(block_id);
            return rc;
        }
        rc = WriteWalRescuing(WalEntry{WalEntryType::Put, op->key, block_id, value_crc, now});
        if (rc != err::kSuccess) {
            dc_.block_allocator.Recycle(block_id);
            return rc;
        }
        ValueMeta old_meta{};
        const bool had_old = (dc_.meta_index.Lookup(op->key, &old_meta) == err::kSuccess);
        ValueMeta meta{};
        meta.block     = block_id;
        meta.timestamp = now;
        meta.crc       = value_crc;
        meta.state     = ValueState::Active;
        dc_.meta_index.Insert(op->key, meta);
        if (had_old) {
            dc_.block_allocator.Recycle(old_meta.block);
            TrimDeviceBlock(old_meta.block);
        }
        MaybeRequestSnapshot();
        return err::kSuccess;
    }

    std::int32_t Reactor::ExecuteDelete(OpNode* op) {
        ValueMeta meta{};
        std::int32_t rc = dc_.meta_index.Lookup(op->key, &meta);
        if (rc != err::kSuccess) return rc;                                  // kIndexKeyNotFound：不存在不写 WAL
        rc = WriteWalRescuing(WalEntry{WalEntryType::Delete, op->key, BlockId{}, 0, util::GetWallTimeNs()});
        if (rc != err::kSuccess) return rc;                                  // WAL 失败不动内存
        dc_.meta_index.Delete(op->key);
        dc_.block_allocator.Recycle(meta.block);
        TrimDeviceBlock(meta.block);
        MaybeRequestSnapshot();
        return err::kSuccess;
    }

    std::int32_t Reactor::ExecuteSetWalLevel(OpNode* op) {
        // 校验（IsValidWalLevel）已在调用线程 fail-fast；执行体假定入参合法。
        const WalLevel old = options_.wal_level;
        // 收紧（攒批档 2/4 → 同步档 1/3）：先把本 reactor 攒批缓冲刷净，新保证从此刻成立。
        if (!IsWalSyncLevel(old) && IsWalSyncLevel(op->new_level)) {
            std::int32_t rc = dc_.wal.Flush();
            if (rc != err::kSuccess) return rc;
        }
        options_.wal_level = op->new_level;   // io/wal 的 opts_ 指此，下次操作现读到
        return err::kSuccess;
    }

    std::int32_t Reactor::ExecuteSnapshot(OpNode* /*op*/) {
        return DoSnapshot();
    }

    // ---- P7M2：快照触发链 + 撞墙救援（从 Engine 迁入，以 dc_/options_ 为隐式对象）----

    void Reactor::TrimDeviceBlock(BlockId id) {
        // TODO(性能轮)：通过待 TRIM 队列异步批量 BLKDISCARD（P7 全程不做）。
        (void)id;
    }

    void Reactor::MaybeRequestSnapshot() {
        const std::uint64_t grown =
            (dc_.wal.last_seq() - dc_.snapshot.last_trigger_seq()) * kWalFrameSize;
        if (grown >= options_.snapshot_threshold_bytes) {
            RequestSnapshot();
        }
    }

    void Reactor::RequestSnapshot() {
        std::int32_t rc = DoSnapshot();
        if (rc != err::kSuccess) {
            CABE_LOG_ERROR("自动触发的快照失败: rc=%d（不影响本次写，后续按退避重试）", rc);
        }
    }

    std::int32_t Reactor::DoSnapshot() {
        dc_.snapshot.NoteTriggerAttempt(dc_.wal.last_seq());
        std::int32_t rc = dc_.wal.Flush();
        if (rc != err::kSuccess) return rc;
        const std::uint64_t covered_seq = dc_.wal.last_seq();
        const std::uint64_t boundary    = dc_.wal.reclaim_boundary();
        rc = dc_.snapshot.Write(covered_seq, [&](const MetaIndexVisitor& v) {
            return dc_.meta_index.ForEach(v);
        });
        if (rc != err::kSuccess) return rc;                                  // 失败：boundary 丢弃，绝不回收
        const std::int32_t rrc = dc_.wal.ReclaimUpTo(boundary);
        if (rrc != err::kSuccess) {
            CABE_LOG_ERROR("WAL 回收失败（快照本身已成功，空间暂不复用）: rc=%d", rrc);
        }
        return err::kSuccess;
    }

    std::int32_t Reactor::WriteWalRescuing(const WalEntry& e) {
        std::int32_t rc = dc_.wal.WriteWal(e);
        if (rc != err::kWalFull) return rc;                                  // 正常路径零开销：就一个比较
        CABE_LOG_WARN("WAL 环已满，强制快照腾空间");
        rc = DoSnapshot();
        if (rc != err::kSuccess) return err::kWalFull;                       // 救不了 → 对外就是"满"
        return dc_.wal.WriteWal(e);                                          // 重试恰一次（Open 验过 ring≥缓冲+4K）
    }

    std::int32_t SubmitAndWait(Reactor& r, OpNode& op) noexcept {
        op.wake = &g_wake_gen;
        const std::uint32_t gen = g_wake_gen.load(std::memory_order_acquire);  // 快照必须在 submit 之前
        r.Submit(&op);
        std::uint32_t cur = gen;
        while (op.result.load(std::memory_order_acquire) == kOpPending) {
            g_wake_gen.wait(cur, std::memory_order_acquire);   // wake_gen==cur 时阻塞
            cur = g_wake_gen.load(std::memory_order_acquire);
        }
        return op.result.load(std::memory_order_acquire);
    }

} // namespace cabe
