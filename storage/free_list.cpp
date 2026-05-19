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
 *   M3 ✅:后台 sort worker 线程(异步 sort)+ lost wakeup 修复
 *         (~FreeList stop_ 写入持 worker_mu_)。
 *   M4 ✅ 本次落地:
 *     - IssueTrim:Release / ReleaseBatch 路径同步 ioctl(BLKDISCARD),
 *       trim_supported_==false 跳过,ioctl 失败 WARN 容错(D-11/D-12)
 *     - Allocate 写保护:全局可用 ≤ max × (1 - reject_ratio) → NO_SPACE
 *     - RebuildFromActive 完整实现(P5 WAL recovery 接口,D-13):
 *       等切换完成 → 过滤越界/去重 active → 重置三容器 → 装载
 *       [0, max) - active(降序)→ 重置状态
 *
 *   设计稿 §8.1 勘误:原写 ioctl(fd, BLKDISCARDGRANULARITY) —— Linux 无
 *   此 ioctl;discard 支持检测改为 Engine.Open 读 sysfs queue/
 *   discard_max_bytes(见 engine.cpp ProbeDiscardSupport),结果经
 *   SetTrimContext 传入。本文件只负责 BLKDISCARD 发放与容错。
 */

#include "free_list.h"

#include "common/logger.h"

#include <algorithm>    // std::sort, std::unique
#include <cerrno>       // errno
#include <cstddef>      // std::size_t
#include <functional>   // std::greater
#include <thread>       // std::this_thread::yield
#include <utility>      // std::swap, std::move
#include <vector>

#include <linux/fs.h>   // BLKDISCARD
#include <sys/ioctl.h>  // ioctl

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
    //   pred 求值与状态修改。TSAN 放大调度窗口后必现。修复:stop_ 写入持
    //   worker_mu_;notify_all 锁外(标准推荐:状态改持锁,notify 锁外,
    //   避免被唤醒线程立即又阻塞在锁上)。
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
    // 因 freelist tuning 参数不合理而失败。Options 入口层(engine_api.cpp
    // ::Engine::Open)做严格校验并返回 InvalidArgument。
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

    // M3 入口:上一轮切换的后台 sort 已完成 → 完成切换。
    //   M2 阶段这是 dead code(同步 sort 已在 StartSwitch 后立即
    //   CompleteSwitch);M3 起 sort 异步,sort_done_ 由后台线程 release-store,
    //   此处 acquire-load 命中后才完成切换 —— M3 真正的完成路径。
    if (state_.load(std::memory_order_acquire) == State::Switching
        && sort_done_.load(std::memory_order_acquire)) {
        CompleteSwitch();
    }

    // M4 写保护:全局可用 ≤ max × (1 - reject_ratio) 时拒绝写入。
    //   available 用三容器之和(= FreeCount 逻辑);M3 切换中 sort_task_/
    //   sort_result_ 被后台短暂持有不计入,available 偏小 → 写保护偏保守
    //   (宁可早拒绝,不会数据错误,设计稿 §13.4 接受切换中 Stats 不精确)。
    //   n=0 fixture:available=0 ≤ 0,返回 NO_SPACE,与"freeList 空"语义一致。
    const uint64_t available =
          static_cast<uint64_t>(containers_[0].size())
        + static_cast<uint64_t>(containers_[1].size())
        + static_cast<uint64_t>(containers_[2].size());
    if (available <= static_cast<uint64_t>(
            static_cast<double>(max_block_count_) * (1.0 - reject_ratio_))) {
        return DEVICE_NO_SPACE;
    }

    if (containers_[free_idx_].empty()) {
        // 切换中且 freeList 耗尽:业务上层重试,等后台 sort 完成后下次
        // Allocate 入口 CompleteSwitch 补充新 freeList(设计稿 §9.1)。
        return DEVICE_NO_SPACE;
    }

    *out = containers_[free_idx_].back();
    containers_[free_idx_].pop_back();

    // M3:pop 后基于 freeList 剩余量检测切换触发(仅 Running 状态)。
    //   StartSwitch 提交 sort 任务后立即返回,不在此完成切换;sort 在
    //   后台异步执行,业务路径只承担 idx swap + notify(~μs)。
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

    try {
        containers_[active_idx_].push_back(id);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }

    // M4:同步发 TRIM(D-11/D-12:不支持设备 / ioctl 失败均容错)。
    //   IssueTrim 在 push_back 之后调:即使 TRIM 失败,blockId 已进
    //   active recycle,FreeList 状态正确;TRIM 仅影响 SSD 内部 WAF。
    IssueTrim(id);

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
        // M4:逐个 TRIM。批量 range 合并优化留后续(M4 先保持简单,
        // ioctl 调用次数 = ids.size();cabe 主场景 Delete 低频可接受)。
        IssueTrim(id);
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
    std::swap(active_idx_, inactive_idx_);
    state_.store(State::Switching, std::memory_order_release);
    switch_pending_.store(false, std::memory_order_release);
}

void FreeList::CompleteSwitch() {
    // M3:在 worker_mu_ 内取回后台已 sort 完成的结果。
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        containers_[inactive_idx_] = std::move(sort_result_);
        sort_result_.clear();
        sort_done_.store(false, std::memory_order_release);
    }

    std::swap(free_idx_, inactive_idx_);
    initial_capacity_ = static_cast<uint64_t>(containers_[free_idx_].size());
    state_.store(State::Running, std::memory_order_release);
    switch_count_.fetch_add(1, std::memory_order_relaxed);
}

void FreeList::MaybeMarkSwitchPending() {
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
    for (;;) {
        std::vector<BlockId> task;
        {
            std::unique_lock<std::mutex> lk(worker_mu_);
            worker_cv_.wait(lk, [this] {
                return stop_.load(std::memory_order_acquire)
                    || !sort_task_.empty();
            });
            if (stop_.load(std::memory_order_acquire)) {
                return;
            }
            task = std::move(sort_task_);
            sort_task_.clear();
        }

        // 锁外执行 sort(业务路径零阻塞)。降序排列配合 freeList 的
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
// M4:TRIM 发放(设计稿 §8.2,D-11/D-12)
// ============================================================

void FreeList::IssueTrim(const BlockId id) const {
    // D-12:不支持 discard 的设备(Engine.Open sysfs 探测得 trim_supported_
    // == false,或 dev_fd_ 无效)直接跳过,功能不受影响。
    if (!trim_supported_ || dev_fd_ < 0) {
        return;
    }
    // BLKDISCARD 入参:uint64_t range[2] = { 字节偏移, 字节长度 }。
    // 每个 BlockId 对应设备上 [id*CHUNK, id*CHUNK + CHUNK) 一个 1 MiB chunk。
    uint64_t range[2] = {
        static_cast<uint64_t>(id) * CABE_VALUE_DATA_SIZE,
        static_cast<uint64_t>(CABE_VALUE_DATA_SIZE),
    };
    if (::ioctl(dev_fd_, BLKDISCARD, range) != 0) {
        // D-11:TRIM 失败不影响数据正确性(blockId 已回 active recycle),
        // 仅 SSD 内部 GC/WAF 优化失效。WARN 暴露问题,不返回错误。
        CABE_LOG_WARN("BLKDISCARD failed for blockId=%lu: errno=%d",
                      static_cast<unsigned long>(id), errno);
    }
}

// ============================================================
// P5 WAL recovery 用(D-13;P4.5 M4 完整实现)
// ============================================================

int32_t FreeList::RebuildFromActive(std::span<const BlockId> active) {
    if (!initialized_) {
        return POOL_NOT_INITIALIZED;
    }

    // 防御性:等待 in-flight 切换完成。recovery 是 Engine.Open 阶段、
    // 无业务并发,通常不会有切换;但若调用方在异常时序下调用,这里
    // 把后台 sort 收尾后再重建,保证三容器状态一致。
    while (state_.load(std::memory_order_acquire) == State::Switching) {
        if (sort_done_.load(std::memory_order_acquire)) {
            CompleteSwitch();
        } else {
            std::this_thread::yield();
        }
    }

    // 过滤越界 BlockId(≥ max 忽略,设计稿 §9.1)+ 排序 + 去重,
    // 保证 sorted_active.size() ≤ max_block_count_,后续求 freeList 大小
    // 不会下溢。
    std::vector<BlockId> sorted_active;
    try {
        sorted_active.reserve(active.size());
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    for (const BlockId b : active) {
        if (b < max_block_count_) {
            sorted_active.push_back(b);
        } else {
            CABE_LOG_WARN("RebuildFromActive: blockId=%lu >= max=%lu, ignored",
                          static_cast<unsigned long>(b),
                          static_cast<unsigned long>(max_block_count_));
        }
    }
    std::sort(sorted_active.begin(), sorted_active.end());
    sorted_active.erase(
        std::unique(sorted_active.begin(), sorted_active.end()),
        sorted_active.end());

    for (auto& c : containers_) {
        c.clear();
    }
    const uint64_t free_n =
        max_block_count_ - static_cast<uint64_t>(sorted_active.size());
    try {
        containers_[0].reserve(free_n);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }

    // 构造 freeList = [0, max) - active,降序排列(pop_back 取最小)。
    // 从高到低扫 [0, max),用 sorted_active 的逆向迭代器同步跳过 active。
    auto rit = sorted_active.rbegin();
    for (uint64_t i = max_block_count_; i > 0; --i) {
        const BlockId b = static_cast<BlockId>(i - 1);
        if (rit != sorted_active.rend() && *rit == b) {
            ++rit;
            continue;
        }
        containers_[0].push_back(b);
    }

    free_idx_         = 0;
    active_idx_       = 1;
    inactive_idx_     = 2;
    initial_capacity_ = static_cast<uint64_t>(containers_[0].size());
    switch_count_.store(0, std::memory_order_relaxed);
    switch_pending_.store(false, std::memory_order_release);
    sort_done_.store(false, std::memory_order_release);
    state_.store(State::Running, std::memory_order_release);

    CABE_LOG_INFO("FreeList rebuilt from active: requested=%zu, "
                  "valid_unique_active=%zu, freeList=%zu",
                  active.size(), sorted_active.size(),
                  containers_[0].size());
    return SUCCESS;
}

// ============================================================
// 观察 / Stats(独立 getter,D-14)
// ============================================================

uint64_t FreeList::FreeCount() const {
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
