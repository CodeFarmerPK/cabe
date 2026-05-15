/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:10
 * Created by: CodeFarmerPK
 *
 * P4.5 M1 改造(2026-05-15):FreeList 三容器轮换骨架。
 *   见 free_list.h 顶部注释与 doc/p4.5_freelist_design.md。
 *
 *   M1 落地范围:
 *     - SetMaxBlockCount 全量装载 [0, n) 到 containers_[0](降序)
 *     - 严格升序 Allocate(pop_back 取最小 BlockId)
 *     - Release / ReleaseBatch 进 active recycle(O(1) push)
 *     - Stats getter(FreeCount / AllocatableCount / PendingRecycleCount 等)
 *     - SetTrimContext / SetTuning 上下文存储
 *
 *   M1 不做(留后续 milestone):
 *     - 切换触发与角色互换(M2)
 *     - 后台 sort worker 线程(M3)
 *     - ioctl(BLKDISCARD) 同步 TRIM(M4)
 *     - 写保护(reject_ratio 拒绝,M4)
 *     - 完整 RebuildFromActive(M4)
 */

#include "free_list.h"

#include "common/logger.h"

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

    // P4.5 M1:从 freeList 单容器分配,不触发切换、不做写保护
    //   - M2 在这里加 ShouldTriggerSwitch / StartSwitch 调用
    //   - M4 在这里加全局写保护(全局可用 ≤ max × (1 - reject_ratio) 拒绝)
    if (containers_[free_idx_].empty()) {
        return DEVICE_NO_SPACE;
    }

    *out = containers_[free_idx_].back();
    containers_[free_idx_].pop_back();
    return SUCCESS;
}

int32_t FreeList::Release(const BlockId id) {
    if (!initialized_) {
        return POOL_NOT_INITIALIZED;
    }

    // P4.5 M1:进 active recycle(无 TRIM,无对称水位检测)
    //   - M2 在这里加对称水位标志(switch_pending_)
    //   - M4 在这里加 IssueTrim 同步发 ioctl(BLKDISCARD)
    try {
        containers_[active_idx_].push_back(id);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
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
    return SUCCESS;
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
    // M1 阶段尚未对接 P5 WAL recovery,此调用返回 CABE_INVALID_DATA_SIZE
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
