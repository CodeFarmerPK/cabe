/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:10
 * Created by: CodeFarmerPK
 *
 * P4.5 改造(2026-05-15):FreeList 三容器轮换 + 严格升序分配 + 异步 sort
 *
 *   分阶段交付(见 doc/p4.5_freelist_design.md §14):
 *     M1 ✅ 数据结构骨架 + Allocate / Release / Stats 基础实现
 *     M2 ✅ 本次落地 — StartSwitch / CompleteSwitch 同步实现 + 状态机激活
 *     M3 异步化   — 后台 sort worker 线程 + 跨线程同步原语
 *     M4 周边设施 — TRIM 集成 + 完整 RebuildFromActive + 写保护 + Options 透传
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
 *   M2 切换状态机(设计稿 §5.3):
 *     Running ──ShouldTriggerSwitch()──→ Switching ──CompleteSwitch()──→ Running
 *     M2 阶段 sort 在调用栈内同步执行(StartSwitch 内立即 sort + 立即
 *     CompleteSwitch);M3 把 sort 移到后台线程,Switching 状态才会持续到
 *     下次业务调用入口。
 *
 *   线程安全契约(D-8,见设计稿 §6.2 + R-14):
 *     - 非 const 方法(Allocate / Release / ReleaseBatch / SetMaxBlockCount
 *       / SetTrimContext / SetTuning / RebuildFromActive)必须在 Engine
 *       unique_lock 下调用
 *     - const 方法(Stats getter)允许在 shared_lock 下调用
 *     - FreeList 不持有业务路径锁,P6 reactor 阶段再细化
 */

#ifndef CABE_FREE_LIST_H
#define CABE_FREE_LIST_H

#include "common/error_code.h"
#include "common/structs.h"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

class FreeList {
public:
    FreeList() = default;
    ~FreeList() = default;

    // FreeList 持有内部状态(P4.5 M3 后还将持有后台线程 + 互斥量),
    // 任何拷贝 / 移动都会破坏唯一所有权。与项目内其他持状态类一致 = delete 纪律。
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
    //   - 同 n 幂等 → SUCCESS
    //   - 不同 n 拒绝 → CABE_INVALID_DATA_SIZE(避免重复 Open 静默改变状态)
    //
    // n = 0 是合法边界(单测 fixture 用),Allocate 立即返回 DEVICE_NO_SPACE。
    //
    // 性能:40T 设备 = 41M BlockId,reserve + push_back ≈ 300 ms。Open 阶段
    // 一次性开销,可接受。
    [[nodiscard]] int32_t SetMaxBlockCount(uint64_t n);

    // 设置 TRIM 上下文(Engine.Open 阶段调,Engine.Close 时无需清理 — Close
    // 会析构 FreeList)。
    //
    // M1 阶段仅存储字段;M4 阶段接入 Release 路径同步 ioctl(BLKDISCARD)。
    void SetTrimContext(int dev_fd, bool trim_supported);

    // 设置可调阈值(Engine.Open 从 Options 透传)。
    // 参数语义:
    //   - switch_ratio       freeList 已用比例触发切换,默认 0.90
    //                        (即 freeList.size() ≤ initial × 0.10 触发)
    //   - reject_ratio       全局已用比例触发写保护,默认 0.90
    //                        (即可用 ≤ max × 0.10 时 Allocate NO_SPACE,M4 接入)
    //   - symmetric_ratio    对称水位触发倍数,默认 1.5
    //                        (active.size() ≥ freeList.size() × 1.5 触发切换)
    //   - min_recycle_threshold  切换前 active 最小 BlockId 数,默认 1024
    //                            (化解 R-NEW-1 启动后死锁风险)
    //
    // M2 起 switch_ratio / symmetric_ratio / min_recycle_threshold 接入切换判定;
    // reject_ratio 写保护在 M4 接入。
    void SetTuning(double switch_ratio,
                   double reject_ratio,
                   double symmetric_ratio,
                   uint64_t min_recycle_threshold);

    // ============================================================
    // 业务核心
    // ============================================================

    // 分配一个 BlockId,严格升序(每次返回 freeList 当前最小者)。
    // M1:freeList 降序 vector pop_back 取尾 = 取最小。
    // M2:入口检测上轮切换 sort 完成 → CompleteSwitch;pop 后检测
    //     ShouldTriggerSwitch → StartSwitch(M2 同步,立即 CompleteSwitch)。
    // M4:加写保护(全局可用 ≤ 10% 拒绝)。
    [[nodiscard]] int32_t Allocate(BlockId* out);

    // 释放一个 BlockId,进 active recycle,等切换后才能再被分配。
    // M1:仅 push 到 active。
    // M2:push 后检测对称水位,达标则标记 switch_pending_(实际切换延迟到
    //     下次 Allocate)。
    // M4:加 ioctl(BLKDISCARD) 同步 TRIM(失败 WARN 不影响返回)。
    [[nodiscard]] int32_t Release(BlockId id);

    // 批量释放,原子语义(D-15):
    //   - reserve 失败 → 整批不 push,recycle 状态不变,返回 MEMORY_INSERT_FAIL
    //   - reserve 成功 → push_back 在 trivially-copyable 类型上 noexcept,整批 SUCCESS
    //
    // M2:批量结束后判定一次对称水位(等价 Release 末尾的 MaybeMarkSwitchPending)。
    //
    // P4.5 改造起不做 double-release 防御性扫描(active 容器在切换间隔可累积到
    // 百万级,O(N) 扫描不可承受;Engine 调用方契约保证 BlockId 不重复)。
    [[nodiscard]] int32_t ReleaseBatch(const std::vector<BlockId>& ids);

    // ============================================================
    // P5 WAL recovery 用(D-13)
    // ============================================================

    // 从 chunkIndex 中所有 active 状态的 chunk 的 blockId 集合反推 freeList。
    // 行为:freeList = [0, max) - active(降序),active / inactive 清空,
    //       switch_count 重置为 0。
    //
    // P4.5 M1 仅提供接口签名(占位返回 CABE_INVALID_DATA_SIZE);M4 完整实现 +
    // 单测覆盖。P5 WAL recovery 后由 Engine.Open 调用一次。
    [[nodiscard]] int32_t RebuildFromActive(std::span<const BlockId> active);

    // ============================================================
    // 观察 / Stats(独立 getter,D-14)
    // ============================================================

    // 全局可用 BlockId 数(三容器之和,兼容旧 FreeCount 语义)。
    // Engine 业务层用此值判断"还能写多少",含义最贴近旧设计。
    [[nodiscard]] uint64_t FreeCount() const;

    // 立即可分配的 BlockId 数(freeList.size() — 立即 Allocate 可拿到的上限)。
    // 与 FreeCount 的差值 = PendingRecycleCount(等切换才可用)。
    [[nodiscard]] uint64_t AllocatableCount() const;

    // 等切换才可分配的 BlockId 数(active + inactive)。
    // 这些 BlockId 物理上未被引用,但暂时不在分配池中。
    [[nodiscard]] uint64_t PendingRecycleCount() const;

    // 累计切换次数(M2 起每次 CompleteSwitch 递增)。
    [[nodiscard]] uint64_t SwitchCount() const;

    // 设备容量上限(SetMaxBlockCount 设置的值)。
    [[nodiscard]] uint64_t MaxBlockCount() const;

    // 设备是否支持 TRIM(SetTrimContext 设置)。
    [[nodiscard]] bool TrimSupported() const;

    // 当前是否处于切换中(state_ == Switching)。
    // M2 同步 sort:此值在 Allocate 调用栈内瞬时为 true,返回时已回 false,
    // 外部观察不到。M3 异步 sort 后才会持续可观察。
    [[nodiscard]] bool IsSortInProgress() const;

private:
    // ============================================================
    // 三容器架构(P4.5 M1 落地,见设计稿 §5.1)
    //
    //   containers_[free_idx_]     = 分配源 freeList(降序存储,pop_back 取最小)
    //   containers_[active_idx_]   = 接收 Release(无序累积)
    //   containers_[inactive_idx_] = 等待 sort 或保持残余
    //
    //   不变式:三个 idx 互不相同,值在 {0, 1, 2} 中
    //
    //   生命周期:三个容器从 SetMaxBlockCount 创建到 FreeList 析构,通过 idx
    //   swap 实现角色变换,数据不动(O(1) 切换,见设计稿 §5.2)。
    // ============================================================
    std::vector<BlockId> containers_[3];
    uint8_t free_idx_     = 0;
    uint8_t active_idx_   = 1;
    uint8_t inactive_idx_ = 2;

    uint64_t max_block_count_  = 0;
    uint64_t initial_capacity_ = 0;   // 当前轮 freeList 启用时容量(切换触发参考)
    bool     initialized_      = false;

    // 可调阈值(默认值匹配设计稿 D-3 / D-6 / D-NEW-1)
    double   switch_ratio_          = 0.90;
    double   reject_ratio_          = 0.90;
    double   symmetric_ratio_       = 1.5;
    uint64_t min_recycle_threshold_ = 1024;

    // 状态机(M2 起开始转换)
    enum class State : uint8_t { Running, Switching };
    std::atomic<State>    state_{State::Running};
    std::atomic<bool>     sort_done_{false};
    std::atomic<bool>     switch_pending_{false};
    std::atomic<uint64_t> switch_count_{0};

    // ============================================================
    // M2 切换缓冲(设计稿 §14 M2.4)
    //   M2 阶段在调用线程内使用(StartSwitch 内 sort,同步);
    //   M3 阶段改为业务线程 set sort_task_ → 后台线程 sort → set sort_result_,
    //   届时这两个容器跨线程访问,由 worker_mu_ 保护(M3 引入)。
    // ============================================================
    std::vector<BlockId> sort_task_;     // 待 sort 的旧 active 内容
    std::vector<BlockId> sort_result_;   // sort 完成的内容,等 CompleteSwitch 取回

    // TRIM 上下文(M1 仅存储,M4 接入 ioctl 调用)
    int  dev_fd_         = -1;
    bool trim_supported_ = false;

    // ============================================================
    // M2 切换内部方法(设计稿 §14 M2.1)
    // ============================================================

    // 三条件复合判定是否触发切换。
    //   前置:active.size() ≥ min_recycle_threshold(R-NEW-1 化解)
    //   触发(任一):freeList.size() ≤ initial_capacity × (1 - switch_ratio)
    //             OR active.size() ≥ freeList.size() × symmetric_ratio
    //             OR switch_pending_(Release 路径已标记)
    [[nodiscard]] bool ShouldTriggerSwitch() const;

    // 启动切换:抢走 active 内容到 sort_task_,swap(active, inactive),
    // state → Switching。M2 在此立即同步 sort 并置 sort_done_;
    // M3 改为 notify 后台 worker 线程,不在此 sort。
    void StartSwitch();

    // 完成切换:sort_result_ → inactive 容器,swap(free, inactive),
    // state → Running,switch_count++。
    void CompleteSwitch();

    // Release / ReleaseBatch 末尾调用:对称水位达标则标记 switch_pending_。
    // 实际切换不在 Release 路径执行(避免 Release 承担切换开销),延迟到
    // 下次 Allocate 检测 switch_pending_ 时触发。
    void MaybeMarkSwitchPending();
};


#endif // CABE_FREE_LIST_H
