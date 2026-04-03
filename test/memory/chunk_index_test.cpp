/*
 * Project: Cabe
 * Created Time: 2026-04-03
 * Created by: CodeFarmerPK
 *
 * ChunkIndex 单元测试
 * 第二层索引: std::map (模拟 B+ 树), chunkId → ChunkMeta{blockId, crc, ...}
 * 核心设计: GetRange — lower_bound 定位 + iterator++ 顺序遍历
 */

#include <gtest/gtest.h>
#include "memory/chunk_index.h"

class ChunkIndexTest : public ::testing::Test {
protected:
    ChunkIndex index_;

    static ChunkMeta MakeMeta(BlockId blockId, uint32_t crc = 0xDEADBEEF) {
        return ChunkMeta{
            .blockId   = blockId,
            .crc       = crc,
            .timestamp = 1000,
            .state     = DataState::Active
        };
    }

    // 批量插入连续 chunkId: [first, first+count)
    void InsertRange(ChunkId first, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            ASSERT_EQ(SUCCESS, index_.Put(first + i, MakeMeta(100 + i)));
        }
    }
};

// ============================================================
// Put / Get 基本操作
// ============================================================

TEST_F(ChunkIndexTest, PutAndGet) {
    ChunkMeta in = MakeMeta(42, 0x12345678);
    ASSERT_EQ(SUCCESS, index_.Put(0, in));

    ChunkMeta out{};
    ASSERT_EQ(SUCCESS, index_.Get(0, &out));
    EXPECT_EQ(42u,         out.blockId);
    EXPECT_EQ(0x12345678u, out.crc);
    EXPECT_EQ(DataState::Active, out.state);
}

TEST_F(ChunkIndexTest, GetNonExistentReturnsNotFound) {
    ChunkMeta out{};
    EXPECT_EQ(CHUNK_NOT_FOUND, index_.Get(999, &out));
}

TEST_F(ChunkIndexTest, GetNullPtrReturnsError) {
    EXPECT_EQ(MEMORY_NULL_POINTER_EXCEPTION, index_.Get(0, nullptr));
}

TEST_F(ChunkIndexTest, PutOverwriteExisting) {
    index_.Put(5, MakeMeta(100));
    index_.Put(5, MakeMeta(200));

    ChunkMeta out{};
    ASSERT_EQ(SUCCESS, index_.Get(5, &out));
    EXPECT_EQ(200u, out.blockId);
}

// ============================================================
// GetRange — 核心设计验证
// ============================================================

TEST_F(ChunkIndexTest, GetRangeBasic) {
    InsertRange(10, 5);  // chunkId 10,11,12,13,14

    std::vector<ChunkMeta> metas;
    ASSERT_EQ(SUCCESS, index_.GetRange(10, 5, &metas));
    ASSERT_EQ(5u, metas.size());

    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(100u + i, metas[i].blockId);
    }
}

TEST_F(ChunkIndexTest, GetRangeSingleChunk) {
    index_.Put(0, MakeMeta(42));

    std::vector<ChunkMeta> metas;
    ASSERT_EQ(SUCCESS, index_.GetRange(0, 1, &metas));
    ASSERT_EQ(1u, metas.size());
    EXPECT_EQ(42u, metas[0].blockId);
}

TEST_F(ChunkIndexTest, GetRangeWithGapReturnsNotFound) {
    // chunkId 0,1,3 — 缺少 2
    index_.Put(0, MakeMeta(100));
    index_.Put(1, MakeMeta(101));
    index_.Put(3, MakeMeta(103));

    std::vector<ChunkMeta> metas;
    EXPECT_EQ(CHUNK_NOT_FOUND, index_.GetRange(0, 4, &metas));
}

TEST_F(ChunkIndexTest, GetRangeNullPtrReturnsError) {
    EXPECT_EQ(MEMORY_NULL_POINTER_EXCEPTION, index_.GetRange(0, 1, nullptr));
}

TEST_F(ChunkIndexTest, GetRangeFromNonExistentStart) {
    InsertRange(10, 5);

    std::vector<ChunkMeta> metas;
    EXPECT_EQ(CHUNK_NOT_FOUND, index_.GetRange(0, 3, &metas));
}

TEST_F(ChunkIndexTest, GetRangeLargeFile) {
    // 模拟大文件: 1000 个连续 chunk
    const uint32_t count = 1000;
    InsertRange(0, count);

    std::vector<ChunkMeta> metas;
    ASSERT_EQ(SUCCESS, index_.GetRange(0, count, &metas));
    ASSERT_EQ(count, static_cast<uint32_t>(metas.size()));

    for (uint32_t i = 0; i < count; ++i) {
        EXPECT_EQ(100u + i, metas[i].blockId);
    }
}

TEST_F(ChunkIndexTest, GetRangeMultipleFilesNonOverlapping) {
    // 文件 A: chunkId [0, 100)
    InsertRange(0, 100);
    // 文件 B: chunkId [100, 150)
    InsertRange(100, 50);

    std::vector<ChunkMeta> metasA;
    ASSERT_EQ(SUCCESS, index_.GetRange(0, 100, &metasA));
    EXPECT_EQ(100u, metasA.size());

    std::vector<ChunkMeta> metasB;
    ASSERT_EQ(SUCCESS, index_.GetRange(100, 50, &metasB));
    EXPECT_EQ(50u, metasB.size());
}

// ============================================================
// 有序性验证: 乱序插入后 GetRange 仍能正确工作
// ============================================================

TEST_F(ChunkIndexTest, InsertionOrderDoesNotAffectRange) {
    index_.Put(3, MakeMeta(103));
    index_.Put(0, MakeMeta(100));
    index_.Put(4, MakeMeta(104));
    index_.Put(1, MakeMeta(101));
    index_.Put(2, MakeMeta(102));

    std::vector<ChunkMeta> metas;
    ASSERT_EQ(SUCCESS, index_.GetRange(0, 5, &metas));
    ASSERT_EQ(5u, metas.size());

    for (uint32_t i = 0; i < 5; ++i) {
        EXPECT_EQ(100u + i, metas[i].blockId);
    }
}

// ============================================================
// Delete（逻辑删除）
// ============================================================

TEST_F(ChunkIndexTest, DeleteSingleChunk) {
    index_.Put(5, MakeMeta(42));
    ASSERT_EQ(SUCCESS, index_.Delete(5));

    ChunkMeta out{};
    EXPECT_EQ(CHUNK_DELETED, index_.Get(5, &out));
    EXPECT_EQ(DataState::Deleted, out.state);
    // blockId 仍可读（用于回收磁盘块）
    EXPECT_EQ(42u, out.blockId);
}

TEST_F(ChunkIndexTest, DeleteNonExistentReturnsNotFound) {
    EXPECT_EQ(CHUNK_NOT_FOUND, index_.Delete(999));
}

TEST_F(ChunkIndexTest, DeleteAlreadyDeletedReturnsDeleted) {
    index_.Put(5, MakeMeta(42));
    index_.Delete(5);
    EXPECT_EQ(CHUNK_DELETED, index_.Delete(5));
}

// ============================================================
// DeleteRange
// ============================================================

TEST_F(ChunkIndexTest, DeleteRangeBasic) {
    InsertRange(0, 5);
    ASSERT_EQ(SUCCESS, index_.DeleteRange(0, 5));

    for (uint32_t i = 0; i < 5; ++i) {
        ChunkMeta out{};
        EXPECT_EQ(CHUNK_DELETED, index_.Get(i, &out));
    }
}

TEST_F(ChunkIndexTest, DeleteRangeWithGapReturnsNotFound) {
    index_.Put(0, MakeMeta(100));
    index_.Put(2, MakeMeta(102));
    // 缺少 chunkId=1
    EXPECT_EQ(CHUNK_NOT_FOUND, index_.DeleteRange(0, 3));
}

TEST_F(ChunkIndexTest, DeleteRangePartiallyApplied) {
    // DeleteRange 遇到 gap 时提前返回，已遍历的节点已被标记
    InsertRange(0, 2);
    // chunkId 0,1 存在，2 不存在
    EXPECT_EQ(CHUNK_NOT_FOUND, index_.DeleteRange(0, 3));

    // 0 和 1 已被标记为 Deleted
    ChunkMeta out{};
    EXPECT_EQ(CHUNK_DELETED, index_.Get(0, &out));
    EXPECT_EQ(CHUNK_DELETED, index_.Get(1, &out));
}

// ============================================================
// Remove（物理移除）
// ============================================================

TEST_F(ChunkIndexTest, RemoveSingleChunk) {
    index_.Put(5, MakeMeta(42));
    ASSERT_EQ(SUCCESS, index_.Remove(5));

    ChunkMeta out{};
    EXPECT_EQ(CHUNK_NOT_FOUND, index_.Get(5, &out));
    EXPECT_EQ(0u, index_.Size());
}

TEST_F(ChunkIndexTest, RemoveNonExistentReturnsNotFound) {
    EXPECT_EQ(CHUNK_NOT_FOUND, index_.Remove(999));
}

// ============================================================
// RemoveRange
// ============================================================

TEST_F(ChunkIndexTest, RemoveRangeBasic) {
    InsertRange(10, 3);
    ASSERT_EQ(SUCCESS, index_.RemoveRange(10, 3));
    EXPECT_EQ(0u, index_.Size());
}

TEST_F(ChunkIndexTest, RemoveRangePartialFailsAndStops) {
    InsertRange(10, 2);
    // 尝试移除 3 个，第 3 个不存在
    EXPECT_EQ(CHUNK_NOT_FOUND, index_.RemoveRange(10, 3));
    // 前 2 个已被移除
    EXPECT_EQ(0u, index_.Size());
}

// ============================================================
// Size
// ============================================================

TEST_F(ChunkIndexTest, SizeTracksCorrectly) {
    EXPECT_EQ(0u, index_.Size());

    InsertRange(0, 10);
    EXPECT_EQ(10u, index_.Size());

    // 逻辑删除不影响 Size
    index_.Delete(5);
    EXPECT_EQ(10u, index_.Size());

    // 物理移除减少 Size
    index_.Remove(5);
    EXPECT_EQ(9u, index_.Size());
}
