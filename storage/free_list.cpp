/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:10
 * Created by: CodeFarmerPK
 *
 * P4.5 改造(2026-05-15):FreeList 三容器轮换。
 *   见 free_list.h 顶部注释与 doc/p4.5_freelist_design.md §14。
 *
 *   M1 ✅ 落地:SetMaxBlockCount 全量装载、严格升序 Allocate、Release 进
 *             active、Stats getter、SetTrimContext / SetTuning 上下文存储。
 *
 *   M2 ✅ 本次落地:
 *     - ShouldTriggerSwitch 三条件复合判定(min_recycle 前置 + freeList
 *       阈值 / 对称水位 / switch_pending)
 *     - StartSwitch / CompleteSwitch 角色互换 + 状态机转换
 *     - M2 阶段 sort 在 StartSwitch 内同步执行(M3 移到后台 worker)
 *     - Allocate 接入切换触发 + 完成检测
 *     - Release / ReleaseBatch 接入对称水位标记
 *
 *   M2 不做(留后续 milestone):
 *     - 后台 sort worker 线程(M3,sort 改异步)
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

int32_t FreeList::SetMaxBlockCount(const uint64_t n) {
    // 多次调用契约(D-18):同 n 幂等,不同 n 拒绝
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
        return SUCCESS;
    }

    // 装载 [0, n) 到 containers_[0],降序存储 — pop_back 取最小实现升序分配
    containers_[0].clear();
    try {
        containers_[0].reserve(n);
    } catch (...) {
        max_block_count_ = 0;
        return MEMORY_INSERT_FAIL;
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

    // M2/M3 入口:上一轮切换的 sort 已完成则完成切换。
    //   M2 阶段 sort 同步执行(StartSwitch 内立即 sort,下方立即 CompleteSwitch),
    //   此分支在 M2 不会命中(Allocate 返回时 state_ 必然已回 Running)。
    //   M3 异步 sort 后,后台线程置 sort_done_,此分支才成为真正的完成入口
    //   —— 这是 M2→M3 平滑过渡的关键:M2 先把入口写好,M3 只需让 sort 异步化
    //   并移除下方"立即 CompleteSwitch"那一行。
    if (state_.load(std::memory_order_acquire) == State::Switching
        && sort_done_.load(std::memory_order_acquire)) {
        CompleteSwitch();
    }

    if (containers_[free_idx_].empty()) {
        return DEVICE_NO_SPACE;
    }

    *out = containers_[free_idx_].back();
    containers_[free_idx_].pop_back();

    // M2:pop 后基于 freeList 剩余量检测切换触发(在 Running 状态下)。
    //   ShouldTriggerSwitch 用 pop 之后的 freeList.size() 判断阈值,语义正确
    //   ("拿走这一个之后还剩多少")。
    if (state_.load(std::memory_order_acquire) == State::Running
        && ShouldTriggerSwitch()) {
        StartSwitch();
        // M2 同步:StartSwitch 内已 sort 完成 + sort_done_=true,
        // 立即 CompleteSwitch(M3 移除此行,改由下次 Allocate 入口检测
        // sort_done_ 触发 —— 见上方入口注释)。
        CompleteSwitch();
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
// M2 切换内部方法(设计稿 §14 M2.1)
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
    // 抢走 active 内容,准备 sort;active 容器随后变 inactive(空)
    sort_task_ = std::move(containers_[active_idx_]);
    containers_[active_idx_].clear();  // move-from vector 状态 unspecified,显式 clear

    // 角色互换:
    //   旧 active 容器(刚 move 走,已空)→ 变 inactive
    //   旧 inactive 容器(可能含上轮残余)→ 变 active,继续接收新 Release
    std::swap(active_idx_, inactive_idx_);

    state_.store(State::Switching, std::memory_order_release);
    sort_done_.store(false, std::memory_order_release);
    switch_pending_.store(false, std::memory_order_release);

    // M2:同步执行 sort(M3 改为 lock worker_mu_ + move sort_task_ +
    // cv.notify_one(),由后台 worker 线程锁外执行 std::sort)。
    // 降序排列(std::greater)以配合 freeList 的 pop_back 取最小语义。
    std::sort(sort_task_.begin(), sort_task_.end(), std::greater<BlockId>());
    sort_result_ = std::move(sort_task_);
    sort_task_.clear();
    sort_done_.store(true, std::memory_order_release);
}

void FreeList::CompleteSwitch() {
    // 已 sort 完成的内容回填到 inactive 容器
    containers_[inactive_idx_] = std::move(sort_result_);
    sort_result_.clear();
    sort_done_.store(false, std::memory_order_release);

    // 角色互换:
    //   旧 inactive 容器(已 sort 完)→ 变 freeList(新分配源)
    //   旧 freeList 容器(含未消费残余)→ 变 inactive,等下一轮 swap 进 active
    std::swap(free_idx_, inactive_idx_);

    initial_capacity_ = static_cast<uint64_t>(containers_[free_idx_].size());
    state_.store(State::Running, std::memory_order_release);
    switch_count_.fetch_add(1, std::memory_order_relaxed);
}

void FreeList::MaybeMarkSwitchPending() {
    // 已在切换中(M3 异步 sort 期间),不重复标记
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

// ============================================================
// P5 WAL recovery 用(D-13;P4.5 M4 完整实现)
// ============================================================

int32_t FreeList::RebuildFromActive(std::span<const BlockId> /*active*/) {
    // P4.5 M1 占位 — 接口签名固化,实现留 M4:
    //   1. 等正在进行的切换完成(M3 后台 sort 完成检测)
    //   2. 排序 active(便于求补集)
    //   3. 重置三容器,装载 [0, max) - active 到 containers_[0](降序)
    //   4. 重置 idx / initial_capacity / switch_count
    //
    // M2 阶段尚未对接 P5 WAL recovery,此调用返回 CABE_INVALID_DATA_SIZE
    // 提醒调用方"尚未实现",避免静默成功导致 recovery 路径无声错误。
    return CABE_INVALID_DATA_SIZE;
}

// ============================================================
// 观察 / Stats(独立 getter,D-14)
// ============================================================

uint64_t FreeList::FreeCount() const {
    // 全局可用 = 三容器之和(兼容旧语义)
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
