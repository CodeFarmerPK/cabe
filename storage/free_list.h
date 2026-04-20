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

    // FreeList 是 Engine 内部的可变状态，复制语义没有意义 ——
    // 两个 FreeList 各自管理同样的 blockId 序列会立刻分裂出冲突。
    // 与 Storage / BufferPool 保持一致的 = delete 纪律，
    // 避免被无意按值传递。
    FreeList(const FreeList&) = delete;
    FreeList& operator=(const FreeList&) = delete;
    FreeList(FreeList&&) = delete;
    FreeList& operator=(FreeList&&) = delete;

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
