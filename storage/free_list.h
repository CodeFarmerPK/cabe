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

    // 配置块数量上限。0 表示无上限(单元测试 fixture 不感知设备容量时使用)。
    //
    // Engine::Open 调用本接口,把 Storage::BlockCount() 直接传进来——这是
    // 裸设备语义下"能写多少 chunk"的硬上限,由 Storage 在 Open 阶段从
    // ioctl(BLKGETSIZE64) 取字节数后向下取整得到。Allocate 走 nextBlockId_++
    // 路径前会校验是否越界,越界返回 DEVICE_NO_SPACE。
    void SetMaxBlockCount(uint64_t maxBlockCount);

    // 分配一个 block。
    // 优先从 freeBlocks_ 复用;空时走 nextBlockId_++,此时若 maxBlockCount_ 已设定
    // 且 nextBlockId_ 已达上限,返回 DEVICE_NO_SPACE。
    int32_t Allocate(BlockId* blockId);

    // 回收一个 block
    int32_t Release(BlockId blockId);

    // 批量回收
    int32_t ReleaseBatch(const std::vector<BlockId>& blockIds);

    // 获取空闲块数量
    size_t FreeCount() const;

    // 获取下一个 blockId
    BlockId NextBlockId() const;

    // 当前配置的块数量上限。0 = 无上限。
    uint64_t MaxBlockCount() const;

private:
    BlockId nextBlockId_ = 0;
    uint64_t maxBlockCount_ = 0; // 0 = 无上限;Engine::Open 设备就绪后写入

    std::vector<BlockId> freeBlocks_;

    //TODO 回收队列
    //std::vector<BlockId> recycledBlocks_;
};


#endif // CABE_FREE_LIST_H
