/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:10
 * Created by: CodeFarmerPK
 *
 * P4.5 改造(2026-05-15):FreeList 三容器轮换。
 *   见 free_list.h 顶部注释与 doc/p4.5_freelist_design.md §14。
 *
 *   M1 ✅:SetMaxBlockCount 全量装载、严格升序 Allocate、Release 进
 *         active、Stats getter、SetTrimContext / SetTuning 上下文存储。
 *   M2 ✅:ShouldTriggerSwitch 三条件 + StartSwitch / CompleteSwitch
 *         角色互换 + 状态机(M2 阶段 sort 同步)。
 *   M3 ✅ 本次落地:
 *     - 后台 sort worker 线程 SortWorkerFn(SetMaxBlockCount 启动 /
 *       ~FreeList join)
 *     - StartSwitch 改为提交 sort_task_ + notify(不在业务线程 sort)
 *     - CompleteSwitch 由 Allocate 入口检测 sort_done_ 后触发(M2 的
 *       "立即 CompleteSwitch" 移除)
 *     - worker_mu_ 保护 sort_task_ / sort_result_ 跨线程交换
 *
 *   M3 不做(留后续 milestone):
 *     - ioctl(BLKDISCARD) 同步 TRIM(M4)
 *     - 写保护(reject_ratio 拒绝,M4)
 *     - 完整 RebuildFromActive(M4)
 */

#include "free_list.h"

#include "common/logger.h"

#include <algorithm>    // std::sort
#include <cstddef>      // std::size_t
#include <functional>   // std::greater
#include <utility>      // std::swap, std::move

// ============================================================
// 生命周期
// ============================================================

FreeList::~FreeList() {
    // M3:停止后台 sort worker。
    //
    // ★ lost wakeup 修复(P4.5 M3):stop_ 的写入必须在 worker_mu_ 锁内,
    //   与 SortWorkerFn 中 worker_cv_.wait(lk, pred) 的 pred 求值(持
    //   worker_mu_)建立 happens-before。否则存在经典竞态:worker 在 wait
    //   内检查 pred(stop_=false、sort_task_ 空)为 false 之后、真正进入 cv
    //   阻塞之前的窗口,~FreeList 的 stop_=true + notify 会丢失(notify 时
    //   worker 尚未进入 cv 阻塞),worker 随后永久阻塞 → join() 死锁。
    //   stop_ 是 atomic 也消除不了此 cv 固有竞态——必须靠 worker_mu_ 同步
    //   pred 求值与状态修改。TSAN 放大调度窗口后必现,WorkerLifecycleNoHang
    //   随机卡住即此因。修复:stop_ 写入持 worker_mu_;notify_all 锁外
    //   (标准推荐:状态改持锁,notify 锁外,避免被唤醒线程立即又阻塞在锁上)。
    //
    // worker 命中 stop_ 后直接 return(即使有 pending sort_task_ 也不处理
    // —— P4.5 不持久化,丢弃切换中 sort 任务无副作用,设计稿 §9.1)。
    // join 在成员逆序析构之前完成,确保 worker 退出后 worker_mu_ /
    // sort_task_ / sort_result_ 析构时无并发。
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        stop_.store(true, std::memory_order_release);
    }
    worker_cv_.notify_all();
    if (sort_worker_.joinable()) {
        sort_worker_.join();
    }
}

int32_t FreeList::SetMaxBlockCount(const uint64_t n) {
    // 多次调用契约(D-18):同 n 幂等(不重复启动 worker),不同 n 拒绝
    if (initialized_) {
        if (n == max_block_count_) {
            return SUCCESS;
        }
        return CABE_INVALID_DATA_SIZE;
    }

    max_block_count_ = n;

    if (n == 0) {
        // 单测 fixture 路径:无容量,Allocate 立即 NO_SPACE
        free_idx_         = 0;
        active_idx_       = 1;
        inactive_idx_     = 2;
        initial_capacity_ = 0;
        initialized_      = true;
        // M3:n=0 也启动 worker,统一析构 join 逻辑(worker 永远 cv.wait
        // 阻塞直到析构 stop_,资源开销 ≈ 一个阻塞线程栈)
        sort_worker_ = std::thread([this] { SortWorkerFn(); });
        return SUCCESS;
    }

    // 装载 [0, n) 到 containers_[0],降序存储 — pop_back 取最小实现升序分配
    containers_[0].clear();
    try {
        containers_[0].reserve(n);
    } catch (...) {
        max_block_count_ = 0;
        return MEMORY_INSERT_FAIL;   // initialized_ 仍 false,可重试,worker 未启动
    }
    for (uint64_t i = n; i > 0; --i) {
        containers_[0].push_back(static_cast<BlockId>(i - 1));
    }
    containers_[1].clear();
    containers_[2].clear();

    free_idx_         = 0;
    active_idx_       = 1;
    inactive_idx_     = 2;
    initial_capacity_ = n;
    initialized_      = true;

    // M3:首次成功初始化后启动后台 sort worker(仅一次)
    sort_worker_ = std::thread([this] { SortWorkerFn(); });

    CABE_LOG_INFO("FreeList initialized: max_block_count=%lu, "
                  "switch_ratio=%.2f, reject_ratio=%.2f, "
                  "symmetric_ratio=%.2f, min_recycle_threshold=%lu",
                  static_cast<unsigned long>(n),
                  switch_ratio_, reject_ratio_, symmetric_ratio_,
                  static_cast<unsigned long>(min_recycle_threshold_));
    return SUCCESS;
}

void FreeList::SetTrimContext(const int dev_fd, const bool trim_supported) {
    dev_fd_         = dev_fd;
    trim_supported_ = trim_supported;
}

void FreeList::SetTuning(const double switch_ratio,
                         const double reject_ratio,
                         const double symmetric_ratio,
                         const uint64_t min_recycle_threshold) {
    // 单字段独立校验:超界值静默忽略(保持已有默认),避免 Engine.Open
    // 因 freelist tuning 参数不合理而失败。Options 入口层做严格校验更合适。
    if (switch_ratio > 0.0 && switch_ratio < 1.0) {
        switch_ratio_ = switch_ratio;
    }
    if (reject_ratio > 0.0 && reject_ratio < 1.0) {
        reject_ratio_ = reject_ratio;
    }
    if (symmetric_ratio > 0.0) {
        symmetric_ratio_ = symmetric_ratio;
    }
    if (min_recycle_threshold >= 1) {
        min_recycle_threshold_ = min_recycle_threshold;
    }
}

// ============================================================
// 业务核心
// ============================================================

int32_t FreeList::Allocate(BlockId* out) {
    if (out == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }
    if (!initialized_) {
        return POOL_NOT_INITIALIZED;
    }

    // M3:上一轮切换的后台 sort 已完成 → 完成切换。
    //   M2 阶段这是 dead code(同步 sort 已在 StartSwitch 后立即
    //   CompleteSwitch);M3 起 sort 异步,sort_done_ 由后台线程 release-store,
    //   此处 acquire-load 命中后才完成切换 —— 这是 M3 真正的完成路径。
    if (state_.load(std::memory_order_acquire) == State::Switching
        && sort_done_.load(std::memory_order_acquire)) {
        CompleteSwitch();
    }

    if (containers_[free_idx_].empty()) {
        // 切换中且 freeList 耗尽:业务上层重试,等后台 sort 完成后下次
        // Allocate 入口 CompleteSwitch 补充新 freeList(设计稿 §9.1)。
        // 40T 设备触发切换时 freeList 仍剩 ~10%(4M),后台 sort ~100ms
        // 内业务最多消耗几百个,不可能耗尽;小规模测试可能触发,测试侧
        // poll 重试处理。
        return DEVICE_NO_SPACE;
    }

    *out = containers_[free_idx_].back();
    containers_[free_idx_].pop_back();

    // M3:pop 后基于 freeList 剩余量检测切换触发(仅 Running 状态)。
    //   StartSwitch 提交 sort 任务后立即返回,不在此完成切换(M2 的
    //   "立即 CompleteSwitch" 已移除);sort 在后台异步执行,业务路径
    //   只承担 StartSwitch 的 idx swap + notify(~μs)。
    if (state_.load(std::memory_order_acquire) == State::Running
        && ShouldTriggerSwitch()) {
        StartSwitch();
    }

    return SUCCESS;
}

int32_t FreeList::Release(const BlockId id) {
    if (!initialized_) {
        return POOL_NOT_INITIALIZED;
    }

    // P4.5 M1/M2:进 active recycle(M4 加 IssueTrim 同步发 ioctl(BLKDISCARD))
    try {
        containers_[active_idx_].push_back(id);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }

    // M2:对称水位检测 → 标记 switch_pending_(实际切换延迟到下次 Allocate)
    MaybeMarkSwitchPending();
    return SUCCESS;
}

int32_t FreeList::ReleaseBatch(const std::vector<BlockId>& ids) {
    if (!initialized_) {
        return POOL_NOT_INITIALIZED;
    }
    if (ids.empty()) {
        return SUCCESS;
    }

    // 原子性(D-15):reserve 失败 → 整批不 push,recycle 状态不变
    try {
        containers_[active_idx_].reserve(
            containers_[active_idx_].size() + ids.size());
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    // reserve 成功后 push_back 对 trivially-copyable BlockId 是 noexcept
    for (const BlockId id : ids) {
        containers_[active_idx_].push_back(id);
    }

    // M2:批量结束后判定一次对称水位(等价 Release 末尾的检测)
    MaybeMarkSwitchPending();
    return SUCCESS;
}

// ============================================================
// 切换内部方法(M2 实现 / M3 改造)
// ============================================================

bool FreeList::ShouldTriggerSwitch() const {
    const std::size_t active_sz = containers_[active_idx_].size();

    // 前置条件(R-NEW-1 化解):active 不够多,切换没意义且会让 freeList
    // 变得过小甚至空 —— 启动后纯写入场景靠这一条避免死锁。
    if (active_sz < min_recycle_threshold_) {
        return false;
    }

    const std::size_t free_sz = containers_[free_idx_].size();

    // 触发条件 1:freeList 已用 (1 - switch_ratio),即剩余 ≤ initial × (1 - r)
    const bool free_threshold =
        static_cast<double>(free_sz)
        <= static_cast<double>(initial_capacity_) * (1.0 - switch_ratio_);

    // 触发条件 2:对称水位 active ≥ free × symmetric_ratio
    const bool symmetric =
        static_cast<double>(active_sz)
        >= static_cast<double>(free_sz) * symmetric_ratio_;

    // 触发条件 3:Release 路径已标记 switch_pending_
    const bool pending = switch_pending_.load(std::memory_order_acquire);

    return free_threshold || symmetric || pending;
}

void FreeList::StartSwitch() {
    // M3:在 worker_mu_ 内把 active 内容交给后台 sort 线程。
    //   sort_task_ 跨线程(后台 take),必须在 worker_mu_ 内 set;
    //   sort_done_ 在锁内重置为 false,确保后台 sort 完 set true 不被覆盖;
    //   notify 唤醒后台线程。
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        sort_task_ = std::move(containers_[active_idx_]);
        containers_[active_idx_].clear();  // move-from 状态 unspecified,显式 clear
        sort_done_.store(false, std::memory_order_release);
        worker_cv_.notify_one();
    }

    // idx swap / state 在 worker_mu_ 外:这些只业务线程访问(Engine 锁串行),
    // 后台线程绝不触碰,无并发。
    //   旧 active 容器(刚 move 走,已空)→ 变 inactive
    //   旧 inactive 容器(可能含上轮残余)→ 变 active,继续接收新 Release
    std::swap(active_idx_, inactive_idx_);
    state_.store(State::Switching, std::memory_order_release);
    switch_pending_.store(false, std::memory_order_release);
}

void FreeList::CompleteSwitch() {
    // M3:在 worker_mu_ 内取回后台已 sort 完成的结果。
    //   sort_result_ 跨线程(后台 set),必须在 worker_mu_ 内 take;
    //   containers_[inactive_idx_] 业务私有,在锁内访问也安全(后台不碰)。
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        containers_[inactive_idx_] = std::move(sort_result_);
        sort_result_.clear();
        sort_done_.store(false, std::memory_order_release);
    }

    // idx swap / state / 计数在 worker_mu_ 外(业务私有,串行)。
    //   旧 inactive 容器(已 sort 完)→ 变 freeList(新分配源)
    //   旧 freeList 容器(含未消费残余)→ 变 inactive,等下一轮 swap 进 active
    std::swap(free_idx_, inactive_idx_);
    initial_capacity_ = static_cast<uint64_t>(containers_[free_idx_].size());
    state_.store(State::Running, std::memory_order_release);
    switch_count_.fetch_add(1, std::memory_order_relaxed);
}

void FreeList::MaybeMarkSwitchPending() {
    // 已在切换中(M3 后台 sort 期间),不重复标记
    if (state_.load(std::memory_order_acquire) != State::Running) {
        return;
    }
    const std::size_t active_sz = containers_[active_idx_].size();
    if (active_sz < min_recycle_threshold_) {
        return;
    }
    const std::size_t free_sz = containers_[free_idx_].size();
    if (static_cast<double>(active_sz)
        >= static_cast<double>(free_sz) * symmetric_ratio_) {
        switch_pending_.store(true, std::memory_order_release);
    }
}

void FreeList::SortWorkerFn() {
    // M3 后台 sort worker 主循环。生命周期由 SetMaxBlockCount 启动、
    // ~FreeList 通过 stop_ + notify + join 终止。
    for (;;) {
        std::vector<BlockId> task;
        {
            std::unique_lock<std::mutex> lk(worker_mu_);
            worker_cv_.wait(lk, [this] {
                return stop_.load(std::memory_order_acquire)
                    || !sort_task_.empty();
            });
            if (stop_.load(std::memory_order_acquire)) {
                // 即使有 pending sort_task_ 也直接退出 —— P4.5 不持久化,
                // 析构丢弃切换中任务无副作用(设计稿 §9.1)。
                return;
            }
            task = std::move(sort_task_);
            sort_task_.clear();
        }

        // 锁外执行 sort(业务路径零阻塞)。降序排列以配合 freeList 的
        // pop_back 取最小语义。std::sort on uint64_t + std::greater 是
        // noexcept,worker 主循环无需 try/catch。
        std::sort(task.begin(), task.end(), std::greater<BlockId>());

        {
            std::lock_guard<std::mutex> lk(worker_mu_);
            sort_result_ = std::move(task);
            sort_done_.store(true, std::memory_order_release);
        }
    }
}

// ============================================================
// P5 WAL recovery 用(D-13;P4.5 M4 完整实现)
// ============================================================

int32_t FreeList::RebuildFromActive(std::span<const BlockId> /*active*/) {
    // P4.5 M1 占位 — 接口签名固化,实现留 M4:
    //   1. 等正在进行的切换完成(后台 sort 完成检测)
    //   2. 排序 active(便于求补集)
    //   3. 重置三容器,装载 [0, max) - active 到 containers_[0](降序)
    //   4. 重置 idx / initial_capacity / switch_count
    //
    // M3 阶段尚未对接 P5 WAL recovery,此调用返回 CABE_INVALID_DATA_SIZE
    // 提醒调用方"尚未实现",避免静默成功导致 recovery 路径无声错误。
    return CABE_INVALID_DATA_SIZE;
}

// ============================================================
// 观察 / Stats(独立 getter,D-14)
// ============================================================

uint64_t FreeList::FreeCount() const {
    // 全局可用 = 三容器之和(兼容旧语义)。M3 切换中后台持有 sort_task_/
    // sort_result_ 的瞬间,这部分不计入(短暂偏小,CompleteSwitch 后恢复);
    // Stats 仅供观测,不要求切换中精确。
    return static_cast<uint64_t>(containers_[0].size())
         + static_cast<uint64_t>(containers_[1].size())
         + static_cast<uint64_t>(containers_[2].size());
}

uint64_t FreeList::AllocatableCount() const {
    return static_cast<uint64_t>(containers_[free_idx_].size());
}

uint64_t FreeList::PendingRecycleCount() const {
    return static_cast<uint64_t>(containers_[active_idx_].size())
         + static_cast<uint64_t>(containers_[inactive_idx_].size());
}

uint64_t FreeList::SwitchCount() const {
    return switch_count_.load(std::memory_order_relaxed);
}

uint64_t FreeList::MaxBlockCount() const {
    return max_block_count_;
}

bool FreeList::TrimSupported() const {
    return trim_supported_;
}

bool FreeList::IsSortInProgress() const {
    return state_.load(std::memory_order_acquire) == State::Switching;
}
