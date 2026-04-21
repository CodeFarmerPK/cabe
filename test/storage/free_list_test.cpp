/*
 * Project: Cabe
 * Created Time: 2026-04-03
 * Created by: CodeFarmerPK
 *
 * FreeList 单元测试
 * 磁盘块分配器: 顺序分配 + 回收复用(LIFO)
 */

#include <gtest/gtest.h>
#include "storage/free_list.h"
#include <set>

class FreeListTest : public ::testing::Test {
protected:
    FreeList freeList_;
};

// ============================================================
// Allocate 基本操作
// ============================================================

TEST_F(FreeListTest, AllocateSequential) {
    BlockId b0, b1, b2;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b0));
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b1));
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b2));

    EXPECT_EQ(0u, b0);
    EXPECT_EQ(1u, b1);
    EXPECT_EQ(2u, b2);
}

TEST_F(FreeListTest, AllocateNullPtrReturnsError) {
    EXPECT_EQ(MEMORY_NULL_POINTER_EXCEPTION, freeList_.Allocate(nullptr));
}

// ============================================================
// Release + 重新分配（LIFO 复用）
// ============================================================

TEST_F(FreeListTest, ReleasedBlockIsReused) {
    BlockId b0;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b0));
    EXPECT_EQ(0u, b0);

    ASSERT_EQ(SUCCESS, freeList_.Release(b0));
    EXPECT_EQ(1u, freeList_.FreeCount());

    BlockId reused;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&reused));
    EXPECT_EQ(b0, reused);
    EXPECT_EQ(0u, freeList_.FreeCount());
}

TEST_F(FreeListTest, ReleaseOrderIsLIFO) {
    BlockId b0, b1, b2;
    freeList_.Allocate(&b0);
    freeList_.Allocate(&b1);
    freeList_.Allocate(&b2);

    freeList_.Release(b0);  // freeBlocks_: [b0]
    freeList_.Release(b2);  // freeBlocks_: [b0, b2]
    EXPECT_EQ(2u, freeList_.FreeCount());

    // vector::back() 先出 b2，再出 b0
    BlockId r1, r2;
    freeList_.Allocate(&r1);
    freeList_.Allocate(&r2);
    EXPECT_EQ(b2, r1);
    EXPECT_EQ(b0, r2);
}

// ============================================================
// ReleaseBatch
// ============================================================

TEST_F(FreeListTest, ReleaseBatch) {
    BlockId b0, b1, b2;
    freeList_.Allocate(&b0);
    freeList_.Allocate(&b1);
    freeList_.Allocate(&b2);

    std::vector<BlockId> batch = {b0, b1, b2};
    ASSERT_EQ(SUCCESS, freeList_.ReleaseBatch(batch));
    EXPECT_EQ(3u, freeList_.FreeCount());
}

TEST_F(FreeListTest, ReleaseBatchEmpty) {
    std::vector<BlockId> empty;
    ASSERT_EQ(SUCCESS, freeList_.ReleaseBatch(empty));
    EXPECT_EQ(0u, freeList_.FreeCount());
}

// batch 内部含重复 blockId → 检测为 double-release
TEST_F(FreeListTest, ReleaseBatchDuplicateWithinBatch) {
    BlockId b0, b1, b2;
    freeList_.Allocate(&b0);
    freeList_.Allocate(&b1);
    freeList_.Allocate(&b2);

    std::vector<BlockId> batch = {b0, b1, b0};  // b0 重复
    EXPECT_EQ(FREE_LIST_DOUBLE_RELEASE, freeList_.ReleaseBatch(batch));
    // 失败后 freeBlocks_ 不应被修改（原子性）
    EXPECT_EQ(0u, freeList_.FreeCount());
}

// batch 中含已在 freeBlocks_ 中的 blockId → 检测为 double-release
TEST_F(FreeListTest, ReleaseBatchDuplicateWithExisting) {
    BlockId b0, b1;
    freeList_.Allocate(&b0);
    freeList_.Allocate(&b1);

    ASSERT_EQ(SUCCESS, freeList_.Release(b0));   // b0 已在 freeBlocks_
    EXPECT_EQ(1u, freeList_.FreeCount());

    std::vector<BlockId> batch = {b1, b0};       // b0 重复
    EXPECT_EQ(FREE_LIST_DOUBLE_RELEASE, freeList_.ReleaseBatch(batch));
    // 失败后 freeBlocks_ 仍只有 b0，b1 未被插入
    EXPECT_EQ(1u, freeList_.FreeCount());
}

// ============================================================
// FreeCount / NextBlockId
// ============================================================

TEST_F(FreeListTest, FreeCountInitiallyZero) {
    EXPECT_EQ(0u, freeList_.FreeCount());
}

TEST_F(FreeListTest, NextBlockIdAdvances) {
    EXPECT_EQ(0u, freeList_.NextBlockId());

    BlockId b;
    freeList_.Allocate(&b);
    EXPECT_EQ(1u, freeList_.NextBlockId());

    freeList_.Allocate(&b);
    EXPECT_EQ(2u, freeList_.NextBlockId());

    // Release 不影响 NextBlockId
    freeList_.Release(b);
    EXPECT_EQ(2u, freeList_.NextBlockId());
}

TEST_F(FreeListTest, ReuseDoesNotAdvanceNextBlockId) {
    BlockId b0;
    freeList_.Allocate(&b0);   // nextBlockId_ = 1
    freeList_.Release(b0);

    BlockId reused;
    freeList_.Allocate(&reused);
    // 复用了 b0，nextBlockId_ 不变
    EXPECT_EQ(1u, freeList_.NextBlockId());
}

// ============================================================
// 压力测试
// ============================================================

TEST_F(FreeListTest, AllocateReleaseStress) {
    constexpr int N = 1000;
    std::vector<BlockId> blocks(N);

    for (int i = 0; i < N; ++i) {
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&blocks[i]));
    }
    EXPECT_EQ(static_cast<BlockId>(N), freeList_.NextBlockId());

    ASSERT_EQ(SUCCESS, freeList_.ReleaseBatch(blocks));
    EXPECT_EQ(static_cast<size_t>(N), freeList_.FreeCount());

    // 重新分配，全部复用
    std::set<BlockId> reusedIds;
    for (int i = 0; i < N; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        reusedIds.insert(b);
    }
    EXPECT_EQ(static_cast<size_t>(N), reusedIds.size());
    // nextBlockId_ 不增长
    EXPECT_EQ(static_cast<BlockId>(N), freeList_.NextBlockId());
}
