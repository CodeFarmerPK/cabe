#ifndef CABE_REACTOR_H
#define CABE_REACTOR_H

// P7M1：reactor 异步机制（读路径）。每个 reactor 独占一份 DeviceContext，单线程事件循环；
// 调用线程投递 op 后挂起，reactor 处理后唤醒。无 mutex。设计依据：doc/P7/P7M1_reactor_skeleton_design.md
//
// 放 engine/（非可插拔数据组件，且独立成库会与 device_context.h 撞循环依赖，见 P7-D3）。

#include "engine/device_context.h"
#include "common/structs.h"

#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>

namespace cabe {

    // op 种类。M1 只 Get/Stop；M2 加 Put/Delete/SetWalLevel/Snapshot。
    enum class OpType : std::uint8_t { Get = 1, Stop = 2 };

    // result 兼完成标志的正数哨兵——和 ≤0 的错误码不撞，不进 error_code.h（reactor 内部约定）。
    inline constexpr std::int32_t kOpPending = 1;

    // 操作描述符。建在调用线程栈上、零堆分配（复刻 P6 WriterNode）。输入用视图不拷贝
    // （同步 API，调用栈内存全程有效）。wake 指向调用线程的 thread_local 唤醒字——与 result
    // 分离，避"最后触碰"UB（reactor 在 result.store 之后绝不再碰 OpNode，notify 打在长寿字上）。
    struct OpNode {
        OpType type;                                   // 由投递方设定
        std::string_view key;                          // Get 输入
        DataBuffer out;                                // Get 输出（caller 的 buffer）
        std::atomic<std::int32_t> result{kOpPending};  // 结果槽 + 完成标志
        OpNode* next = nullptr;                        // MPSC 侵入式链
        std::atomic<std::uint32_t>* wake = nullptr;    // → caller 的 thread_local 唤醒字
    };

    // reactor：独占一份 DeviceContext，单线程跑事件循环。不可拷贝、不可移动（内含线程引用 this）
    // → Engine 用 vector<unique_ptr<Reactor>> 持有。
    class Reactor {
    public:
        explicit Reactor(DeviceContext&& dc);
        ~Reactor();

        Reactor(const Reactor&) = delete;
        Reactor& operator=(const Reactor&) = delete;
        Reactor(Reactor&&) = delete;
        Reactor& operator=(Reactor&&) = delete;

        std::int32_t Start();                          // 起工作线程跑 Run；线程创建失败返错误码
        std::int32_t Stop();                           // 投 Stop op + join + 返回关闭首错（drain-then-close）
        void Submit(OpNode* op) noexcept;              // 入队 + 唤醒 reactor（不等待）

    private:
        void Run();                                    // 事件循环
        std::int32_t ExecuteGet(OpNode* op);           // Get 执行体（作用在 dc_ 上）
        void Finalize(OpNode* op, std::int32_t rc) noexcept;  // 回填 result(release) + 唤醒（扣 last-touch）
        void FailWakeChain(OpNode* head) noexcept;     // 掉队 op 全部回 kEngineNotOpen
        void CloseDc() noexcept;                       // 关 snapshot→wal→io，记首错入 close_result_
        OpNode* DrainAll() noexcept;                   // exchange(nullptr, acquire) 整批摘
        static OpNode* Reverse(OpNode* head) noexcept; // LIFO → 到达序

        std::atomic<OpNode*> inbox_{nullptr};          // 栈顶，兼 reactor 空闲时的 futex 字
        std::thread thread_;
        DeviceContext dc_;
        std::int32_t close_result_ = 0;                // Run 退出前写，Stop join 后读（0 = kSuccess）
        bool dc_closed_ = false;                       // RAII 防双关
    };

    // 调用线程侧：投递单个 op 并挂起等结果，返回结果码。用每调用线程一个 thread_local 唤醒字
    // （内部）。M4 fan-out（一个调用等 N 个 reactor）将另加多 op 版本，Submit 本身不变。
    std::int32_t SubmitAndWait(Reactor& r, OpNode& op) noexcept;

} // namespace cabe

#endif // CABE_REACTOR_H
