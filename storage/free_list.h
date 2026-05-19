/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:10
 * Created by: CodeFarmerPK
 *
 * P4.5 改造(2026-05-15):FreeList 三容器轮换 + 严格升序分配 + 异步 sort
 *
 *   分阶段交付(见 doc/p4.5_freelist_design.md §14):
 *     M1 ✅ 数据结构骨架 + Allocate / Release / Stats 基础实现
 *     M2 ✅ StartSwitch / CompleteSwitch 同步实现 + 状态机激活
 *     M3 ✅ 本次落地 — 后台 sort worker 线程 + 跨线程同步原语(sort 异步化)
 *     M4 ✅ 本次落地 — TRIM 集成 + 完整 RebuildFromActive + 写保护 + Options 透传
 *     M5 收尾     — 旧测试调整 + Engine 集成测试 + bench 归档
 *
 *   架构总览(完整论述见设计稿 §5):
 *     三个固定 vector<BlockId> 容器,通过 free_idx_ / active_idx_ /
 *     inactive_idx_ 标识角色;切换时仅交换 idx,数据不动(O(1) 角色互换)。
 *
 *   分配语义:freeList 降序存储,pop_back 取最小 BlockId,保证严格升序分配
 *     (用户决策 D-2,匹配 NVMe 顺序写偏好;长期运行后等价 LIFO,但工程上
 *     可预测,详见设计稿 §2.2)。
 *
 *   M3 切换协议(设计稿 §5.3 / §14 M3.4):
 *     Running ──ShouldTriggerSwitch()──→ Switching ──CompleteSwitch()──→ Running
 *
 *     业务线程 StartSwitch():worker_mu_ 内 move active→sort_task_ + notify,
 *       worker_mu_ 外 swap(active,inactive) + state→Switching。立即返回,
 *       sort 不在业务线程做。
 *     后台线程 SortWorkerFn():cv.wait → 抢 sort_task_ → 锁外 std::sort →
 *       回填 sort_result_ + sort_done_=true。
 *     业务线程下次 Allocate 入口:state==Switching && sort_done_ →
 *       CompleteSwitch()(worker_mu_ 内取 sort_result_,锁外 swap idx)。
 *
 *   跨线程边界(TSAN 关键,设计稿 §5.4 + R-14):
 *     - 后台线程只访问 sort_task_ / sort_result_(worker_mu_ 保护)、
 *       sort_done_ / stop_(atomic acquire/release)、worker_cv_
 *     - 后台线程绝不访问 containers_ / *_idx_ / state_ / switch_count_ /
 *       switch_pending_ / initial_capacity_(业务线程私有,Engine 锁串行)
 *     - 锁层级:Engine 锁 > worker_mu_,单向获取,无死锁可能
 *
 *   线程安全契约(D-8,见设计稿 §6.2 + R-14):
 *     - 非 const 方法(Allocate / Release / ReleaseBatch / SetMaxBlockCount
 *       / SetTrimContext / SetTuning / RebuildFromActive)必须在 Engine
 *       unique_lock 下调用(业务线程串行)
 *     - const 方法(Stats getter)允许在 shared_lock 下调用
 *     - FreeList 内部仅 worker_mu_ 用于业务线程 ↔ 后台 sort 线程协作
 */

#ifndef CABE_FREE_LIST_H
#define CABE_FREE_LIST_H

#include "common/error_code.h"
#include "common/structs.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

class FreeList {
public:
    FreeList() = default;
    // M3:析构停止后台 sort worker(stop_ + notify + join),不再 = default。
    ~FreeList();

    // FreeList 持有后台线程 + 互斥量 + 条件变量,任何拷贝 / 移动都会破坏
    // 唯一所有权。与项目内其他持状态类一致 = delete 纪律。
    FreeList(const FreeList&) = delete;
    FreeList& operator=(const FreeList&) = delete;
    FreeList(FreeList&&) = delete;
    FreeList& operator=(FreeList&&) = delete;

    // ============================================================
    // 生命周期
    // ============================================================

    // 设置设备容量上限并全量装载 [0, n) 到 freeList(降序存储)。
    // 必须在 Allocate / Release 之前调用一次。
    //
    // 多次调用契约(D-18):
    //   - 同 n 幂等 → SUCCESS(不重复启动 worker)
    //   - 不同 n 拒绝 → CABE_INVALID_DATA_SIZE
    //
    // n = 0 是合法边界(单测 fixture 用),Allocate 立即返回 DEVICE_NO_SPACE。
    //
    // M3:首次成功初始化(含 n=0)时启动后台 sort worker 线程,统一析构
    // join 逻辑。worker 在无切换任务时 cv.wait 阻塞,CPU 占用 ≈ 0。
    //
    // 性能:40T 设备 = 41M BlockId,reserve + push_back ≈ 300 ms。Open 阶段
    // 一次性开销,可接受。
    [[nodiscard]] int32_t SetMaxBlockCount(uint64_t n);

    // 设置 TRIM 上下文(Engine.Open 阶段调)。M1 仅存储字段;M4 接入
    // Release 路径同步 ioctl(BLKDISCARD)。
    void SetTrimContext(int dev_fd, bool trim_supported);

    // 设置可调阈值(Engine.Open 从 Options 透传)。
    //   - switch_ratio       freeList 已用比例触发切换,默认 0.90
    //   - reject_ratio       全局已用比例触发写保护,默认 0.90(M4 接入)
    //   - symmetric_ratio    对称水位触发倍数,默认 1.5
    //   - min_recycle_threshold  切换前 active 最小 BlockId 数,默认 1024
    //                            (化解 R-NEW-1 启动后死锁风险)
    void SetTuning(double switch_ratio,
                   double reject_ratio,
                   double symmetric_ratio,
                   uint64_t min_recycle_threshold);

    // ============================================================
    // 业务核心(必须在 Engine unique_lock 下调用)
    // ============================================================

    // 分配一个 BlockId,严格升序(每次返回 freeList 当前最小者)。
    // M3:入口检测后台 sort 已完成 → CompleteSwitch;pop 后检测
    //     ShouldTriggerSwitch → StartSwitch(异步,sort 交后台线程,
    //     不在此完成切换)。
    // 切换中且 freeList 耗尽 → DEVICE_NO_SPACE(业务上层重试,设计稿 §9.1)。
    [[nodiscard]] int32_t Allocate(BlockId* out);

    // 释放一个 BlockId,进 active recycle,等切换后才能再被分配。
    // M2:push 后检测对称水位,达标则标记 switch_pending_。
    // M4:加 ioctl(BLKDISCARD) 同步 TRIM。
    [[nodiscard]] int32_t Release(BlockId id);

    // 批量释放,原子语义(D-15):reserve 失败整批不 push。
    [[nodiscard]] int32_t ReleaseBatch(const std::vector<BlockId>& ids);

    // ============================================================
    // P5 WAL recovery 用(D-13;M4 完整实现,M1/M2/M3 占位)
    // ============================================================

    [[nodiscard]] int32_t RebuildFromActive(std::span<const BlockId> active);

    // ============================================================
    // 观察 / Stats(独立 getter,D-14)
    // ============================================================

    // 全局可用(三容器之和,兼容旧语义)。M3 注意:切换中后台持有
    // sort_task_/sort_result_ 的瞬间,这部分数据不在 containers_ 内,
    // FreeCount 会短暂偏小;CompleteSwitch 后恢复。Stats 仅供观测,
    // 不要求切换中精确(设计稿 §14 M4 描述)。
    [[nodiscard]] uint64_t FreeCount() const;

    [[nodiscard]] uint64_t AllocatableCount() const;
    [[nodiscard]] uint64_t PendingRecycleCount() const;
    [[nodiscard]] uint64_t SwitchCount() const;
    [[nodiscard]] uint64_t MaxBlockCount() const;
    [[nodiscard]] bool TrimSupported() const;

    // 当前是否处于切换中(state_ == Switching)。
    // M3 异步 sort:StartSwitch 后到 CompleteSwitch 前持续为 true,
    // 外部可观察(M2 同步时不可观察)。
    [[nodiscard]] bool IsSortInProgress() const;

private:
    // ============================================================
    // 三容器架构(M1,见设计稿 §5.1)。containers_ / *_idx_ 仅业务线程
    // 访问(Engine 锁串行),后台 sort 线程绝不触碰。
    // ============================================================
    std::vector<BlockId> containers_[3];
    uint8_t free_idx_     = 0;
    uint8_t active_idx_   = 1;
    uint8_t inactive_idx_ = 2;

    uint64_t max_block_count_  = 0;
    uint64_t initial_capacity_ = 0;
    bool     initialized_      = false;

    double   switch_ratio_          = 0.90;
    double   reject_ratio_          = 0.90;
    double   symmetric_ratio_       = 1.5;
    uint64_t min_recycle_threshold_ = 1024;

    // 状态机(M2 起转换)。state_ / switch_count_ / switch_pending_ 仅
    // 业务线程读写;sort_done_ 由后台线程 release-store、业务线程
    // acquire-load。
    enum class State : uint8_t { Running, Switching };
    std::atomic<State>    state_{State::Running};
    std::atomic<bool>     sort_done_{false};
    std::atomic<bool>     switch_pending_{false};
    std::atomic<uint64_t> switch_count_{0};

    // ============================================================
    // 切换 sort 缓冲(M2 引入,M3 起跨线程,worker_mu_ 保护)
    //   业务线程 StartSwitch:worker_mu_ 内 set sort_task_
    //   后台线程 SortWorkerFn:worker_mu_ 内 take sort_task_ → 锁外 sort
    //                          → worker_mu_ 内 set sort_result_
    //   业务线程 CompleteSwitch:worker_mu_ 内 take sort_result_
    // ============================================================
    std::vector<BlockId> sort_task_;
    std::vector<BlockId> sort_result_;

    // TRIM 上下文(M1 仅存储,M4 接入 ioctl)
    int  dev_fd_         = -1;
    bool trim_supported_ = false;

    // ============================================================
    // M3 后台 sort worker(设计稿 §14 M3.1)
    //   sort_worker_ 声明在末尾:析构时 ~FreeList 体内先 join,再按声明
    //   逆序析构成员,join 已确保 worker 退出,其余成员析构时无并发。
    // ============================================================
    std::mutex              worker_mu_;
    std::condition_variable worker_cv_;
    std::atomic<bool>       stop_{false};
    std::thread             sort_worker_;

    // ============================================================
    // 切换内部方法(M2 实现 / M3 改造)
    // ============================================================

    // 三条件复合判定(M2):前置 active ≥ min_recycle_threshold;
    // 触发 freeList 阈值 OR 对称水位 OR switch_pending。
    [[nodiscard]] bool ShouldTriggerSwitch() const;

    // M3:worker_mu_ 内 move active→sort_task_ + sort_done_=false +
    // cv.notify_one;worker_mu_ 外 swap(active,inactive) + state→Switching。
    // 不在此 sort(交后台线程)。
    void StartSwitch();

    // M3:worker_mu_ 内 take sort_result_→inactive 容器 + sort_done_=false;
    // worker_mu_ 外 swap(free,inactive) + state→Running + switch_count++。
    void CompleteSwitch();

    // Release / ReleaseBatch 末尾调用:对称水位达标则标记 switch_pending_。
    void MaybeMarkSwitchPending();

    // M3 后台线程主循环:cv.wait(stop_ || !sort_task_.empty())→ 抢 task →
    // 锁外 std::sort(降序)→ 回填 sort_result_ + sort_done_=true。
    void SortWorkerFn();

    // P4.5 M4:对单个 BlockId 发 ioctl(BLKDISCARD)(SSD TRIM)。
    //   - trim_supported_==false 或 dev_fd_<0 → 直接 return(D-12:不支持
    //     设备由 Engine.Open sysfs 探测,此处跳过)
    //   - ioctl 失败 → CABE_LOG_WARN 容错,不返回错误(D-11:TRIM 失败
    //     不影响数据正确性,blockId 已回 active recycle,仅 SSD WAF 优化失效)
    // const:仅读 dev_fd_/trim_supported_,不改 FreeList 状态;由 Release/
    // ReleaseBatch(非 const)在 push_back 之后调用。
    void IssueTrim(BlockId id) const;
};


#endif // CABE_FREE_LIST_H
