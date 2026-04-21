/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:10
 * Created by: CodeFarmerPK
 */

#include "free_list.h"

int32_t FreeList::Allocate(BlockId* blockId) {
    if (blockId == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }

    if (!freeBlocks_.empty()) {
        *blockId = freeBlocks_.back();
        freeBlocks_.pop_back();
    } else {
        *blockId = nextBlockId_++;
    }

    return SUCCESS;
}

int32_t FreeList::Release(const BlockId blockId) {
    // Double-release 检测：同一 blockId 出现两次会让两次 Allocate 返回同一
    // 物理块 → 两个 key 共享磁盘块 → 静默数据损坏。
    // O(N) 扫描，N = freeBlocks_.size()（正常运行时较小，可接受）。
    for (const BlockId existing : freeBlocks_) {
        if (existing == blockId) {
            return FREE_LIST_DOUBLE_RELEASE;
        }
    }
    // 先 reserve 保证容量，之后的 push_back 对 trivially-copyable
    // 类型（BlockId = uint64_t）是 noexcept。bad_alloc 只可能在 reserve
    // 阶段抛出，此时 blockId 还没被吞，返回错误后调用方能决定重试或
    // 泄漏，不会出现"放进来一半"的中间状态。
    try {
        freeBlocks_.reserve(freeBlocks_.size() + 1);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    freeBlocks_.push_back(blockId); // noexcept for BlockId
    return SUCCESS;
}

int32_t FreeList::ReleaseBatch(const std::vector<BlockId>& blockIds) {
    if (blockIds.empty()) {
        return SUCCESS;
    }

    // 先完整验证，再 reserve + insert，保证原子性：
    //   - 与已有 freeBlocks_ 无重叠（batch 外 double-release）
    //   - batch 内部无重复（batch 内 double-release）
    // 与 Release() 的 O(N) 扫描保持一致的防护强度。
    for (size_t i = 0; i < blockIds.size(); ++i) {
        for (const BlockId existing : freeBlocks_) {
            if (existing == blockIds[i]) return FREE_LIST_DOUBLE_RELEASE;
        }
        for (size_t j = i + 1; j < blockIds.size(); ++j) {
            if (blockIds[i] == blockIds[j]) return FREE_LIST_DOUBLE_RELEASE;
        }
    }

    try {
        freeBlocks_.reserve(freeBlocks_.size() + blockIds.size());
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    freeBlocks_.insert(freeBlocks_.end(), blockIds.begin(), blockIds.end());
    return SUCCESS;
}

size_t FreeList::FreeCount() const {
    return freeBlocks_.size();
}

BlockId FreeList::NextBlockId() const {
    return nextBlockId_;
}
