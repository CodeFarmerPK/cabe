/*
 * Project: Cabe
 * Created Time: 2026-04-03
 * Created by: CodeFarmerPK
 *
 * MetaIndex 单元测试
 * 第一层索引: HashMap, key(string) → KeyMeta{firstChunkId, chunkCount, ...}
 */

#include <gtest/gtest.h>
#include "memory/meta_index.h"

class MetaIndexTest : public ::testing::Test {
protected:
    MetaIndex index_;

    static KeyMeta MakeKeyMeta(ChunkId firstChunkId, uint32_t chunkCount,
                               uint64_t totalSize, DataState state = DataState::Active) {
        return KeyMeta{
            .firstChunkId = firstChunkId,
            .chunkCount   = chunkCount,
            .totalSize    = totalSize,
            .createdAt    = 1000,
            .modifiedAt   = 1000,
            .state        = state
        };
    }
};

// ============================================================
// Put / Get 基本操作
// ============================================================

TEST_F(MetaIndexTest, PutAndGet) {
    KeyMeta in = MakeKeyMeta(0, 1, 512);
    ASSERT_EQ(SUCCESS, index_.Put("key1", in));

    KeyMeta out{};
    ASSERT_EQ(SUCCESS, index_.Get("key1", &out));
    EXPECT_EQ(0u,   out.firstChunkId);
    EXPECT_EQ(1u,   out.chunkCount);
    EXPECT_EQ(512u, out.totalSize);
    EXPECT_EQ(DataState::Active, out.state);
}

TEST_F(MetaIndexTest, PutOverwriteUpdatesValue) {
    index_.Put("key1", MakeKeyMeta(0, 1, 100));
    index_.Put("key1", MakeKeyMeta(10, 5, 5000));

    KeyMeta out{};
    ASSERT_EQ(SUCCESS, index_.Get("key1", &out));
    EXPECT_EQ(10u,   out.firstChunkId);
    EXPECT_EQ(5u,    out.chunkCount);
    EXPECT_EQ(5000u, out.totalSize);
}

TEST_F(MetaIndexTest, GetNonExistentReturnsNotFound) {
    KeyMeta out{};
    EXPECT_EQ(INDEX_KEY_NOT_FOUND, index_.Get("missing", &out));
}

TEST_F(MetaIndexTest, GetNullPtrReturnsError) {
    EXPECT_EQ(MEMORY_NULL_POINTER_EXCEPTION, index_.Get("key1", nullptr));
}

// ============================================================
// Get 对已删除 key 的行为
// 关键语义: Get 在返回 INDEX_KEY_DELETED 之前会填充 *meta
// Engine::Remove 依赖此行为获取已删除 key 的 chunkId 范围
// ============================================================

TEST_F(MetaIndexTest, GetDeletedKeyFillsMetaBeforeReturningDeleted) {
    index_.Put("key1", MakeKeyMeta(100, 8, 8000));
    index_.Delete("key1");

    KeyMeta out{};
    int32_t ret = index_.Get("key1", &out);
    EXPECT_EQ(INDEX_KEY_DELETED, ret);
    // meta 内容必须已被填充
    EXPECT_EQ(100u, out.firstChunkId);
    EXPECT_EQ(8u,   out.chunkCount);
    EXPECT_EQ(8000u, out.totalSize);
    EXPECT_EQ(DataState::Deleted, out.state);
}

// ============================================================
// Delete（逻辑删除）
// ============================================================

TEST_F(MetaIndexTest, DeleteMarksAsDeleted) {
    index_.Put("key1", MakeKeyMeta(0, 1, 100));
    ASSERT_EQ(SUCCESS, index_.Delete("key1"));

    KeyMeta out{};
    EXPECT_EQ(INDEX_KEY_DELETED, index_.Get("key1", &out));
    EXPECT_EQ(DataState::Deleted, out.state);
}

TEST_F(MetaIndexTest, DeleteNonExistentKeyReturnsNotFound) {
    EXPECT_EQ(INDEX_KEY_NOT_FOUND, index_.Delete("missing"));
}

TEST_F(MetaIndexTest, DeleteAlreadyDeletedReturnsDeleted) {
    index_.Put("key1", MakeKeyMeta(0, 1, 100));
    index_.Delete("key1");
    EXPECT_EQ(INDEX_KEY_DELETED, index_.Delete("key1"));
}

// ============================================================
// Remove（物理移除）
// ============================================================

TEST_F(MetaIndexTest, RemoveErasesKey) {
    index_.Put("key1", MakeKeyMeta(0, 1, 100));
    ASSERT_EQ(SUCCESS, index_.Remove("key1"));

    KeyMeta out{};
    EXPECT_EQ(INDEX_KEY_NOT_FOUND, index_.Get("key1", &out));
    EXPECT_EQ(0u, index_.Size());
}

TEST_F(MetaIndexTest, RemoveNonExistentReturnsNotFound) {
    EXPECT_EQ(INDEX_KEY_NOT_FOUND, index_.Remove("missing"));
}

TEST_F(MetaIndexTest, RemoveAfterDeleteSucceeds) {
    index_.Put("key1", MakeKeyMeta(0, 1, 100));
    index_.Delete("key1");
    ASSERT_EQ(SUCCESS, index_.Remove("key1"));
    EXPECT_EQ(0u, index_.Size());
}

// ============================================================
// Size / Contains
// ============================================================

TEST_F(MetaIndexTest, SizeTracksInsertAndRemove) {
    EXPECT_EQ(0u, index_.Size());

    index_.Put("a", MakeKeyMeta(0, 1, 100));
    index_.Put("b", MakeKeyMeta(1, 1, 100));
    EXPECT_EQ(2u, index_.Size());

    // Delete 是逻辑删除，activeCount_ 立即递减，Size 也减少
    index_.Delete("a");
    EXPECT_EQ(1u, index_.Size());

    // Remove 物理删除："a" 已是 Deleted 状态，activeCount_ 不再变动
    index_.Remove("a");
    EXPECT_EQ(1u, index_.Size());
}

TEST_F(MetaIndexTest, ContainsOnlyTrueForActive) {
    index_.Put("key1", MakeKeyMeta(0, 1, 100));
    EXPECT_TRUE(index_.Contains("key1"));

    index_.Delete("key1");
    EXPECT_FALSE(index_.Contains("key1"));

    EXPECT_FALSE(index_.Contains("nonexistent"));
}

// ============================================================
// 多 key 场景
// ============================================================

TEST_F(MetaIndexTest, MultipleKeysIndependent) {
    index_.Put("file_a", MakeKeyMeta(0,  10, 10000));
    index_.Put("file_b", MakeKeyMeta(10, 5,  5000));
    index_.Put("file_c", MakeKeyMeta(15, 1,  100));
    EXPECT_EQ(3u, index_.Size());

    index_.Delete("file_b");
    EXPECT_TRUE(index_.Contains("file_a"));
    EXPECT_FALSE(index_.Contains("file_b"));
    EXPECT_TRUE(index_.Contains("file_c"));

    KeyMeta out{};
    ASSERT_EQ(SUCCESS, index_.Get("file_a", &out));
    EXPECT_EQ(10u, out.chunkCount);
}

// ============================================================
// 大文件 KeyMeta 验证
// ============================================================

TEST_F(MetaIndexTest, LargeFileKeyMeta) {
    // 模拟 1TB 文件: 1048576 个 1MB chunk
    const uint32_t chunkCount = 1048576;
    const uint64_t totalSize  = static_cast<uint64_t>(chunkCount) * CABE_VALUE_DATA_SIZE;
    index_.Put("huge_file", MakeKeyMeta(0, chunkCount, totalSize));

    KeyMeta out{};
    ASSERT_EQ(SUCCESS, index_.Get("huge_file", &out));
    EXPECT_EQ(0u,          out.firstChunkId);
    EXPECT_EQ(chunkCount,  out.chunkCount);
    EXPECT_EQ(totalSize,   out.totalSize);
}
