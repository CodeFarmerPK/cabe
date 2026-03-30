/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:10
 * Created by: CodeFarmerPK
 */

#ifndef CABE_FREE_LIST_H
#define CABE_FREE_LIST_H

#include "common/error_code.h"
#include "common/structs.h"
#include <vector>
#include <cstdint>

class FreeList {
public:
    FreeList() = default;
    ~FreeList() = default;

    // 分配一个 block
    int32_t Allocate(BlockId* blockId);

    // 回收一个 block
    int32_t Release(BlockId blockId);

    // 批量回收
    int32_t ReleaseBatch(const std::vector<BlockId>& blockIds);

    // 获取空闲块数量
    size_t FreeCount() const;

    // 获取下一个 blockId
    BlockId NextBlockId() const;

private:
    BlockId nextBlockId_ = 0;

    std::vector<BlockId> freeBlocks_;

    //TODO 回收队列
    //std::vector<BlockId> recycledBlocks_;
};


#endif // CABE_FREE_LIST_H
