#include "engine/reactor.h"
#include "common/error_code.h"
#include "common/logger.h"
#include "util/crc32.h"

#include <cstring>
#include <utility>

namespace cabe {

    namespace {
        // 每调用线程一个唤醒字，跨多个 reactor 共享（为 M4 fan-out 准备）。生命周期 = 线程，
        // 比 per-op 栈节点活得久 → reactor 的 notify 绝不打在已析构的 OpNode 上（避"最后触碰"UB）。
        thread_local std::atomic<std::uint32_t> g_wake_gen{0};
    } // namespace

    Reactor::Reactor(DeviceContext&& dc) : dc_(std::move(dc)) {}

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
        OpNode stop{OpType::Stop, {}, {}};               // 栈上；join 即同步，Stop op 不需要 wake
        Submit(&stop);
        thread_.join();
        return close_result_;
    }

    void Reactor::Submit(OpNode* op) noexcept {
        OpNode* old = inbox_.load(std::memory_order_relaxed);
        do {
            op->next = old;
        } while (!inbox_.compare_exchange_weak(old, op,
                     std::memory_order_release, std::memory_order_relaxed));
        inbox_.notify_one();   // 唤醒 reactor（栈顶唯一等待者）
    }

    OpNode* Reactor::DrainAll() noexcept {
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
                const std::int32_t rc = ExecuteGet(op);
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
