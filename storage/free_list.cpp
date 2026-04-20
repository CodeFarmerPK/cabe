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
    // 先 reserve 保证容量，之后的 push_back 对 trivially-copyable
    // 类型（BlockId = uint64_t）是 noexcept。bad_alloc 只可能在 reserve
    // 阶段抛出，此时 blockId 还没被吞，返回错误后调用方能决定重试或
    // 泄漏，不会出现"放进来一半"的中间状态。
    try {
        freeBlocks_.reserve(freeBlocks_.size() + 1);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    freeBlocks_.push_back(blockId);   // noexcept for BlockId
    return SUCCESS;
}

int32_t FreeList::ReleaseBatch(const std::vector<BlockId>& blockIds) {
    // 同上：先整体 reserve，再一次性 insert，避免 insert 中途
    // bad_alloc 造成"前半被插入、后半丢失"的部分失败。
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