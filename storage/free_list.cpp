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
    try {
        freeBlocks_.push_back(blockId);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    return SUCCESS;
}

int32_t FreeList::ReleaseBatch(const std::vector<BlockId>& blockIds) {
    try {
        freeBlocks_.insert(freeBlocks_.end(), blockIds.begin(), blockIds.end());
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    return SUCCESS;
}

size_t FreeList::FreeCount() const {
    return freeBlocks_.size();
}

BlockId FreeList::NextBlockId() const {
    return nextBlockId_;
}