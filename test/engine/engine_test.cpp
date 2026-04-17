/*
 * Project: Cabe
 * Created Time: 2026-04-03
 * Created by: CodeFarmerPK
 *
 * Engine 集成测试
 * 依赖 Storage 的 O_DIRECT，需要在支持 direct IO 的文件系统上运行
 * （ext4, xfs 等；tmpfs 不支持）
 * 不支持时相关测试自动 SKIP
 */

    #include <gtest/gtest.h>
#include "engine/engine.h"
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

static bool SupportsDirectIO(const char* path) {
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    // 预分配 64MB 空间
    if (ftruncate(fd, 64 * 1024 * 1024) < 0) {
        ::close(fd);
        return false;
    }
    ::close(fd);

    fd = ::open(path, O_RDWR | O_DIRECT | O_SYNC);
    if (fd < 0) {
        ::unlink(path);
        return false;
    }
    ::close(fd);
    return true;
}

class EngineTest : public ::testing::Test {
protected:
    Engine engine_;
    std::string devicePath_;

    void SetUp() override {
        // 唯一路径: test_suite + test_name + pid
        // 防止 ctest -j N 时多个测试并行走同一文件，在 SetUp() 里
        // 通过 O_TRUNC 互相清零对方的数据
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        devicePath_ = std::string("/var/tmp/cabe_")
                    + info->test_suite_name() + "_" + info->name()
                    + "_" + std::to_string(::getpid()) + ".dat";

        if (!SupportsDirectIO(devicePath_.c_str())) {
            GTEST_SKIP() << "O_DIRECT not supported at " << devicePath_;
        }
    }

    void TearDown() override {
        if (engine_.IsOpen()) {
            engine_.Close();
        }
                    ::unlink(devicePath_.c_str());
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
    EXPECT_EQ(DEVICE_FAILED_TO_OPEN_DEVICE,
              engine_.Put("key1", {data.data(), data.size()}));

    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;
    EXPECT_EQ(DEVICE_FAILED_TO_OPEN_DEVICE,
              engine_.Get("key1", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(DEVICE_FAILED_TO_OPEN_DEVICE, engine_.Delete("key1"));
    EXPECT_EQ(DEVICE_FAILED_TO_OPEN_DEVICE, engine_.Remove("key1"));
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

TEST_F(EngineTest, RemoveMultiChunkFile) {
    ASSERT_EQ(SUCCESS, engine_.Open(devicePath_.c_str()));

    // 2.5MB => 3 个 chunk
    const size_t totalSize = CABE_VALUE_DATA_SIZE * 2 + CABE_VALUE_DATA_SIZE / 2;
    auto data = MakeData(totalSize, 'R');
    ASSERT_EQ(SUCCESS, engine_.Put("multi", {data.data(), data.size()}));
    ASSERT_EQ(SUCCESS, engine_.Remove("multi"));
    EXPECT_EQ(0u, engine_.Size());
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
