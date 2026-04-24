/*
 * Project: Cabe
 * Created Time: 2026-04-03
 * Created by: CodeFarmerPK
 *
 * Engine 集成测试(裸设备语义)
 *
 * Cabe 是直接操作裸块设备的存储引擎,测试需要一个真实的块设备节点。
 * 通过环境变量 CABE_TEST_DEVICE 指定:
 *   export CABE_TEST_DEVICE=/dev/loop0   # 由 scripts/mkloop.sh 创建
 * 未设置时所有 Engine 测试 SKIP。
 *
 * 注意:测试串行执行(ctest --parallel 1)——同一设备不能被多 test 并发占用,
 * 否则 nextBlockId_ / FreeList 会互相覆盖。fixture 内的多线程并发测试
 * 不受此限制(在同一 Engine 实例下)。
 *
 * 数据隔离:每个 test 的 SetUp 都新建 Engine 并 Open 同一设备,内存
 * 索引(MetaIndex / ChunkIndex / FreeList)从空开始,nextBlockId_ 从 0
 * 开始覆盖写之前测试用过的设备区域。每个 test 的 key 互不冲突,
 * 即使读到旧 block 数据也不会被 metaIndex 找到。
 */

#include <gtest/gtest.h>
#include "engine/engine.h"
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
    // 读取环境变量 CABE_TEST_DEVICE,空字符串视作未设置。
    std::string GetTestDevice() {
        const char* env = std::getenv("CABE_TEST_DEVICE");
        if (env == nullptr || *env == '\0') {
            return {};
        }
        return env;
    }
}

class EngineTest : public ::testing::Test {
protected:
    Engine engine_;
    std::string devicePath_;

    void SetUp() override {
        devicePath_ = GetTestDevice();
        if (devicePath_.empty()) {
            GTEST_SKIP() << "CABE_TEST_DEVICE not set; "
                            "use scripts/mkloop.sh to create a loop device "
                            "and `export CABE_TEST_DEVICE=/dev/loopX`";
        }
    }

    void TearDown() override {
        if (engine_.IsOpen()) {
            engine_.Close();
        }
        // 不 unlink:裸设备节点由 sysadmin / mkloop.sh 管理,Engine 全程无写删权
    }

    static std::vector<char> MakeData(size_t size, char fill = 'A') {
        return std::vector<char>(size, fill);
    }
};

// ============================================================
// Open / Close
// ============================================================

TEST_F(EngineTest, OpenAndClose) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    EXPECT_TRUE(engine_.IsOpen());

    ASSERT_EQ(SUCCESS, engine_.Close());
    EXPECT_FALSE(engine_.IsOpen());
}

TEST_F(EngineTest, DoubleOpenIdempotent) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    EXPECT_TRUE(engine_.IsOpen());
}

TEST_F(EngineTest, OpenWithDifferentPathRejected) {
    // 已打开后用不同路径再次 Open 必须被拒绝,避免用户以为路径切换成功
    // 实际仍指向旧设备造成静默写错盘。想真正切换必须先 Close。
    //
    // 内部 ::Engine::Open 在 isOpen_ 状态下用不同 path 直接返回错误码,
    // 不会真去 open(2) 第二个路径——所以这里的 "/dev/null" 只是个不同
    // 字符串,不需要真存在/真是块设备。
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    const std::string otherPath = "/dev/null";
    EXPECT_EQ(ENGINE_ALREADY_OPEN, engine_.Open(otherPath));

    // Engine 仍以原路径打开
    EXPECT_TRUE(engine_.IsOpen());
}

// 同 path,不同 buffer_pool_count:幂等分支必须两个参数都一致才算同一次 Open。
// 不然新 pool 参数会被静默忽略,用户以为调整了并发深度实际没生效。
TEST_F(EngineTest, OpenSamePathDifferentPoolCountRejected) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str(), 8));
    EXPECT_EQ(ENGINE_ALREADY_OPEN, engine_.Open(devicePath_.c_str(), 16));
    // 仍以首次配置保持打开
    EXPECT_TRUE(engine_.IsOpen());
}

// 同 path + 同 pool:必须幂等 SUCCESS(不能被新加的 ALREADY_OPEN 守卫误伤)
TEST_F(EngineTest, OpenSamePathSamePoolCountIdempotent) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str(), 8));
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str(), 8));
    EXPECT_TRUE(engine_.IsOpen());
}

// Close 后在同实例上再 Open 必须被拒绝——内存索引不会被 Close 重置,
// 复用实例会让 metaIndex 与新设备内容错位,静默 corruption。
// 想重开必须销毁此实例构造新的(公开 API 通过 unique_ptr 自然满足此约束)。
TEST_F(EngineTest, OpenAfterCloseRejected) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    ASSERT_EQ(SUCCESS, engine_.Close());
    EXPECT_FALSE(engine_.IsOpen());

    EXPECT_EQ(ENGINE_INSTANCE_USED, engine_.Open(devicePath_.c_str()));
    // 仍然处于 closed 状态(Open 被拒没有副作用)
    EXPECT_FALSE(engine_.IsOpen());
}

TEST_F(EngineTest, DoubleCloseIdempotent) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    ASSERT_EQ(SUCCESS, engine_.Close());
    ASSERT_EQ(SUCCESS, engine_.Close());
}

TEST_F(EngineTest, OpenEmptyPathFails) {
    EXPECT_NE(SUCCESS, engine_.Open(""));
}

// ============================================================
// Put / Get: 单 chunk（data <= 1MB）
// ============================================================

TEST_F(EngineTest, PutGetSmallValue) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    auto data = MakeData(1024, 'X');
    ASSERT_EQ(SUCCESS, engine_.Put("key1", {data.data(), data.size()}));

    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("key1", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(1024u, readSize);
    EXPECT_EQ(0, std::memcmp(data.data(), buf.data(), 1024));
}

TEST_F(EngineTest, PutGetExactlyOneMB) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    auto data = MakeData(CABE_VALUE_DATA_SIZE, 'M');
    ASSERT_EQ(SUCCESS, engine_.Put("exact1mb", {data.data(), data.size()}));

    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("exact1mb", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), readSize);
    EXPECT_EQ(0, std::memcmp(data.data(), buf.data(), CABE_VALUE_DATA_SIZE));
}

// ============================================================
// Put / Get: 多 chunk（data > 1MB）
// ============================================================

TEST_F(EngineTest, PutGetMultiChunk) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    // 3.5 MB => 4 个 chunk
    const size_t totalSize = CABE_VALUE_DATA_SIZE * 3 + CABE_VALUE_DATA_SIZE / 2;
    std::vector<char> data(totalSize);
    for (size_t i = 0; i < totalSize; ++i) {
        data[i] = static_cast<char>(i % 256);
    }

    ASSERT_EQ(SUCCESS, engine_.Put("bigfile", {data.data(), data.size()}));

    std::vector<char> buf(totalSize + CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("bigfile", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(totalSize, readSize);
    EXPECT_EQ(0, std::memcmp(data.data(), buf.data(), totalSize));
}

// ============================================================
// 参数校验
// ============================================================

TEST_F(EngineTest, PutEmptyKeyFails) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    auto data = MakeData(100);
    EXPECT_EQ(CABE_EMPTY_KEY, engine_.Put("", {data.data(), data.size()}));
}

TEST_F(EngineTest, PutEmptyValueFails) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    EXPECT_EQ(CABE_EMPTY_VALUE, engine_.Put("key1", DataView{}));
}

TEST_F(EngineTest, GetNonExistentKey) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;
    EXPECT_EQ(INDEX_KEY_NOT_FOUND,
              engine_.Get("missing", {buf.data(), buf.size()}, &readSize));
}

TEST_F(EngineTest, GetBufferTooSmall) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    auto data = MakeData(1024);
    ASSERT_EQ(SUCCESS, engine_.Put("key1", {data.data(), data.size()}));

    std::vector<char> small(100);
    uint64_t readSize = 0;
    EXPECT_EQ(CABE_INVALID_DATA_SIZE,
              engine_.Get("key1", {small.data(), small.size()}, &readSize));
}

TEST_F(EngineTest, GetNullReadSizeFails) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    auto data = MakeData(100);
    engine_.Put("key1", {data.data(), data.size()});

    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    EXPECT_EQ(MEMORY_NULL_POINTER_EXCEPTION,
              engine_.Get("key1", {buf.data(), buf.size()}, nullptr));
}

TEST_F(EngineTest, OperationsBeforeOpenFail) {
    auto data = MakeData(100);
    EXPECT_EQ(ENGINE_NOT_OPEN,
              engine_.Put("key1", {data.data(), data.size()}));

    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;
    EXPECT_EQ(ENGINE_NOT_OPEN,
              engine_.Get("key1", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(ENGINE_NOT_OPEN, engine_.Delete("key1"));
    EXPECT_EQ(ENGINE_NOT_OPEN, engine_.Remove("key1"));
}

// ============================================================
// Delete（逻辑删除）
// ============================================================

TEST_F(EngineTest, DeleteThenGetReturnsDeleted) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    auto data = MakeData(512);
    ASSERT_EQ(SUCCESS, engine_.Put("key1", {data.data(), data.size()}));
    ASSERT_EQ(SUCCESS, engine_.Delete("key1"));

    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;
    EXPECT_EQ(INDEX_KEY_DELETED,
              engine_.Get("key1", {buf.data(), buf.size()}, &readSize));
}

TEST_F(EngineTest, DeleteNonExistentFails) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    EXPECT_EQ(INDEX_KEY_NOT_FOUND, engine_.Delete("missing"));
}

TEST_F(EngineTest, DeleteEmptyKeyFails) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    EXPECT_EQ(CABE_EMPTY_KEY, engine_.Delete(""));
}

// ============================================================
// Remove（物理移除 + 回收磁盘块）
// ============================================================

TEST_F(EngineTest, RemoveDirectly) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    auto data = MakeData(512);
    ASSERT_EQ(SUCCESS, engine_.Put("key1", {data.data(), data.size()}));
    EXPECT_EQ(1u, engine_.Size());

    ASSERT_EQ(SUCCESS, engine_.Remove("key1"));
    EXPECT_EQ(0u, engine_.Size());

    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;
    EXPECT_EQ(INDEX_KEY_NOT_FOUND,
              engine_.Get("key1", {buf.data(), buf.size()}, &readSize));
}

TEST_F(EngineTest, RemoveAfterDelete) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    auto data = MakeData(512);
    ASSERT_EQ(SUCCESS, engine_.Put("key1", {data.data(), data.size()}));
    ASSERT_EQ(SUCCESS, engine_.Delete("key1"));
    ASSERT_EQ(SUCCESS, engine_.Remove("key1"));
    EXPECT_EQ(0u, engine_.Size());
}

TEST_F(EngineTest, RemoveNonExistentFails) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    EXPECT_EQ(INDEX_KEY_NOT_FOUND, engine_.Remove("missing"));
}

TEST_F(EngineTest, RemoveEmptyKeyFails) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    EXPECT_EQ(CABE_EMPTY_KEY, engine_.Remove(""));
}

TEST_F(EngineTest, RemoveMultiChunkFile) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    // 2.5MB => 3 个 chunk
    const size_t totalSize = CABE_VALUE_DATA_SIZE * 2 + CABE_VALUE_DATA_SIZE / 2;
    auto data = MakeData(totalSize, 'R');
    ASSERT_EQ(SUCCESS, engine_.Put("multi", {data.data(), data.size()}));
    ASSERT_EQ(SUCCESS, engine_.Remove("multi"));
    EXPECT_EQ(0u, engine_.Size());
}

// Put 同一 key 两次:覆盖写应当回收旧 chunks/blocks,否则反复 Put
// 会线性泄漏设备空间。裸设备上 FreeList 有硬上限,不像 regular file
// 那样会被 ext4/btrfs 自动扩展——泄漏立即触发 DEVICE_NO_SPACE。
// 补强校验:Get 出来的内容必须是最后一次 Put 的内容。
TEST_F(EngineTest, PutOverwriteSameKeyRecyclesOldChunks) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    const size_t valueSize = CABE_VALUE_DATA_SIZE;  // 1 MiB
    auto first  = MakeData(valueSize, 'F');
    auto second = MakeData(valueSize, 'S');

    // 第一次 Put
    ASSERT_EQ(SUCCESS, engine_.Put("k", {first.data(), first.size()}));
    EXPECT_EQ(1u, engine_.Size());

    // 覆盖写：旧 chunks/blocks 应被回收
    ASSERT_EQ(SUCCESS, engine_.Put("k", {second.data(), second.size()}));
    EXPECT_EQ(1u, engine_.Size());  // Size 仍然是 1（同 key 覆盖）

    // 读到的应是新内容
    std::vector<char> buf(valueSize);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("k", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(valueSize, readSize);
    EXPECT_EQ(0, std::memcmp(second.data(), buf.data(), valueSize));

    // 反复覆盖 50 次 1 MiB,若 FreeList 正确回收旧 block,nextBlockId_
    // 不会增长,总占用稳定在 ~1 MiB。若泄漏,裸设备达到容量上限后
    // Allocate 会直接返回 DEVICE_NO_SPACE,Put 失败——无 ext4/btrfs
    // 自扩展兜底,泄漏立即可见。
    for (int i = 0; i < 50; ++i) {
        auto data = MakeData(valueSize, static_cast<char>('A' + (i % 26)));
        ASSERT_EQ(SUCCESS, engine_.Put("k", {data.data(), data.size()}))
            << "Put failed at iter " << i;
    }
    EXPECT_EQ(1u, engine_.Size());
}

// 覆盖写：新旧 chunk count 不同（小覆盖大）。
// Put 一个 4 chunk 的 value → Put 同 key 一个 1 chunk 的 value。
// 旧的 4 chunks 全部要被 cleanup 段回收（GetRange + ReleaseBatch + RemoveRange
// 都要正确处理 4-chunk 范围）。读出来的应是新内容。
TEST_F(EngineTest, PutOverwriteShrinkRecyclesAllOldChunks) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    const size_t bigSize = CABE_VALUE_DATA_SIZE * 4;       // 4 chunks
    const size_t smallSize = CABE_VALUE_DATA_SIZE / 4;     // 1 chunk (256 KiB)

    auto big   = MakeData(bigSize, 'B');
    auto small = MakeData(smallSize, 'S');

    ASSERT_EQ(SUCCESS, engine_.Put("k", {big.data(), big.size()}));
    ASSERT_EQ(SUCCESS, engine_.Put("k", {small.data(), small.size()}));

    EXPECT_EQ(1u, engine_.Size());

    std::vector<char> buf(smallSize);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("k", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(smallSize, readSize);
    EXPECT_EQ(0, std::memcmp(small.data(), buf.data(), smallSize));
}

// 覆盖写：大覆盖小（1 chunk → 4 chunks）。
// 旧的 1 chunk 要被回收，新的 4 chunks 写入。考验 cleanup 段对 count=1
// 的 GetRange/RemoveRange 与主写循环的 4-chunk 路径并行正确。
TEST_F(EngineTest, PutOverwriteGrowRecyclesAllOldChunks) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    const size_t smallSize = CABE_VALUE_DATA_SIZE / 4;
    const size_t bigSize   = CABE_VALUE_DATA_SIZE * 4;

    auto small = MakeData(smallSize, 'S');
    auto big   = MakeData(bigSize, 'B');

    ASSERT_EQ(SUCCESS, engine_.Put("k", {small.data(), small.size()}));
    ASSERT_EQ(SUCCESS, engine_.Put("k", {big.data(), big.size()}));

    EXPECT_EQ(1u, engine_.Size());

    std::vector<char> buf(bigSize);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("k", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(bigSize, readSize);
    EXPECT_EQ(0, std::memcmp(big.data(), buf.data(), bigSize));
}

// Delete + Put 同 key 也应回收旧 chunks（被 Delete 标记的旧 chunks
// 仍占着 chunkIndex 条目和 blocks，覆盖 Put 时也要清理）。
TEST_F(EngineTest, PutAfterDeleteRecyclesOldChunks) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    const size_t valueSize = CABE_VALUE_DATA_SIZE;
    auto first  = MakeData(valueSize, 'F');
    auto second = MakeData(valueSize, 'S');

    ASSERT_EQ(SUCCESS, engine_.Put("k", {first.data(), first.size()}));
    ASSERT_EQ(SUCCESS, engine_.Delete("k"));
    // 此时旧 chunks 是 Deleted 状态但仍在 chunkIndex 里

    // 覆盖 Put 应当清理 Delete 残留
    ASSERT_EQ(SUCCESS, engine_.Put("k", {second.data(), second.size()}));

    std::vector<char> buf(valueSize);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("k", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(0, std::memcmp(second.data(), buf.data(), valueSize));
}

// 验证 Remove 的 block 回收 + 复用路径：
// Put A 占用一批 block → Remove A → Put B 应当复用 A 释放出来的 block（LIFO）。
// 间接验证 Remove 正确调用了 freeList_.ReleaseBatch 且 RemoveRange 没有
// 把 block 悄悄保留。
TEST_F(EngineTest, RemoveRecyclesBlocksForReuse) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    // Put 一个 3-chunk value，消耗 chunkId 0..2、blockId 0..2
    const size_t totalSize = CABE_VALUE_DATA_SIZE * 3;
    auto data_a = MakeData(totalSize, 'A');
    ASSERT_EQ(SUCCESS, engine_.Put("A", {data_a.data(), data_a.size()}));

    // Remove A → blockId 0..2 进入 freeList
    ASSERT_EQ(SUCCESS, engine_.Remove("A"));

    // Put B，同样 3 个 chunk → 应复用 A 的 blockId，而不是申请新的
    // （LIFO 栈顺序下拿到 blockId 2, 1, 0）。此处只验证读写正确。
    auto data_b = MakeData(totalSize, 'B');
    ASSERT_EQ(SUCCESS, engine_.Put("B", {data_b.data(), data_b.size()}));

    std::vector<char> buf(totalSize);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("B", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(totalSize, readSize);
    EXPECT_EQ(0, std::memcmp(data_b.data(), buf.data(), totalSize));
}

// ============================================================
// 多 key 场景
// ============================================================

TEST_F(EngineTest, MultipleKeysPutAndGet) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    auto data1 = MakeData(100, 'A');
    auto data2 = MakeData(200, 'B');
    auto data3 = MakeData(300, 'C');

    ASSERT_EQ(SUCCESS, engine_.Put("key1", {data1.data(), data1.size()}));
    ASSERT_EQ(SUCCESS, engine_.Put("key2", {data2.data(), data2.size()}));
    ASSERT_EQ(SUCCESS, engine_.Put("key3", {data3.data(), data3.size()}));
    EXPECT_EQ(3u, engine_.Size());

    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;

    ASSERT_EQ(SUCCESS, engine_.Get("key1", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(100u, readSize);
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ('A', buf[i]);
    }

    ASSERT_EQ(SUCCESS, engine_.Get("key2", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(200u, readSize);
    for (size_t i = 0; i < 200; ++i) {
        EXPECT_EQ('B', buf[i]);
    }

    ASSERT_EQ(SUCCESS, engine_.Get("key3", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(300u, readSize);
    for (size_t i = 0; i < 300; ++i) {
        EXPECT_EQ('C', buf[i]);
    }
}

// ============================================================
// Size
// ============================================================

TEST_F(EngineTest, SizeTracksCorrectly) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    EXPECT_EQ(0u, engine_.Size());

    auto data = MakeData(100);
    engine_.Put("a", {data.data(), data.size()});
    EXPECT_EQ(1u, engine_.Size());

    engine_.Put("b", {data.data(), data.size()});
    EXPECT_EQ(2u, engine_.Size());

    engine_.Remove("a");
    EXPECT_EQ(1u, engine_.Size());
}

// Delete 是逻辑删除，Size() 不应计入已删除但未 Remove 的条目。
// 曾因 Size() 返回 indexMap_.size()（含 Deleted 条目）而存在 bug。
TEST_F(EngineTest, SizeDoesNotCountDeletedKeys) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));
    auto data = MakeData(100);

    engine_.Put("a", {data.data(), data.size()});
    engine_.Put("b", {data.data(), data.size()});
    EXPECT_EQ(2u, engine_.Size());

    engine_.Delete("a");
    // Delete 是逻辑删除：Size 应立即降为 1，而非保持 2
    EXPECT_EQ(1u, engine_.Size());

    engine_.Remove("a");
    EXPECT_EQ(1u, engine_.Size());  // Remove 后仍是 1（b 还在）

    engine_.Delete("b");
    EXPECT_EQ(0u, engine_.Size());  // 全部逻辑删除，Size 应为 0
}

// ============================================================
// 数据完整性: 写入后读出，字节级校验
// ============================================================

TEST_F(EngineTest, DataIntegrityMultiChunk) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    // 构造具有特征 pattern 的 2MB 数据
    const size_t totalSize = CABE_VALUE_DATA_SIZE * 2;
    std::vector<char> data(totalSize);
    for (size_t i = 0; i < totalSize; ++i) {
        data[i] = static_cast<char>((i * 7 + 13) % 256);
    }

    ASSERT_EQ(SUCCESS, engine_.Put("integrity", {data.data(), data.size()}));

    std::vector<char> buf(totalSize);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("integrity", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(totalSize, readSize);

    // 逐字节校验
    for (size_t i = 0; i < totalSize; ++i) {
        EXPECT_EQ(data[i], buf[i]) << "Mismatch at byte " << i;
    }
}

// 末 chunk 半满的 CRC 契约验证：
// Put 和 Get 两侧都基于 keyMeta.totalSize 推导 chunkSize，要求它们
// 覆盖完全相同的字节范围。此用例选了 3.01 MiB —— 4 chunks，末 chunk
// 只有 ~10 KiB 有效数据 + 其余 padding zero。任一侧推导错位都会让
// CRC 不匹配，返回 DATA_CRC_MISMATCH。
TEST_F(EngineTest, PutGetOddTailSize) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    const size_t totalSize = CABE_VALUE_DATA_SIZE * 3 + 10 * 1024; // 3 MiB + 10 KiB
    std::vector<char> data(totalSize);
    // 避开全零 pattern：尾部 padding 和数据要有明显差异
    for (size_t i = 0; i < totalSize; ++i) {
        data[i] = static_cast<char>((i * 131 + 7) & 0xFF);
    }

    ASSERT_EQ(SUCCESS, engine_.Put("odd_tail", {data.data(), data.size()}));

    std::vector<char> buf(totalSize);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("odd_tail", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(totalSize, readSize);

    EXPECT_EQ(0, std::memcmp(data.data(), buf.data(), totalSize));
}
