/*
 * Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 *
 * P2 公开 API 集成测试
 *
 * 覆盖范围：
 *   - Status 类（纯内存，无 I/O）
 *   - cabe::Engine 生命周期（Open / Close / IsOpen）
 *   - Put / Get / Delete 的功能语义和错误路径
 *   - 大 value 多 chunk 读写
 *   - Size() 计数语义
 *
 * 依赖：Storage 使用 O_DIRECT，需要在支持 direct I/O 的文件系统上运行
 *       （ext4、xfs 等；tmpfs 不支持）。不支持时 Engine 相关测试自动 SKIP。
 */

#include <gtest/gtest.h>
#include "cabe/cabe.h"
#include "common/structs.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

// ============================================================
// Status 单元测试（无 I/O，无 fixture）
// ============================================================

TEST(StatusTest, DefaultIsOK) {
    cabe::Status s;
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(s.code(), cabe::Status::Code::kOK);
    EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, OKFactory) {
    const cabe::Status s = cabe::Status::OK();
    EXPECT_TRUE(s.ok());
    EXPECT_FALSE(s.IsNotFound());
}

TEST(StatusTest, NotFound) {
    const cabe::Status s = cabe::Status::NotFound("missing key");
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.IsNotFound());
    EXPECT_EQ(s.code(), cabe::Status::Code::kNotFound);
    EXPECT_EQ(s.ToString(), "NotFound: missing key");
}

TEST(StatusTest, NotFoundNoMessage) {
    const cabe::Status s = cabe::Status::NotFound();
    EXPECT_TRUE(s.IsNotFound());
    EXPECT_EQ(s.ToString(), "NotFound");
}

TEST(StatusTest, InvalidArgument) {
    const cabe::Status s = cabe::Status::InvalidArgument("key is empty");
    EXPECT_TRUE(s.IsInvalidArgument());
    EXPECT_EQ(s.ToString(), "InvalidArgument: key is empty");
}

TEST(StatusTest, IOError) {
    const cabe::Status s = cabe::Status::IOError("disk failure");
    EXPECT_TRUE(s.IsIOError());
    EXPECT_EQ(s.ToString(), "IOError: disk failure");
}

TEST(StatusTest, Corruption) {
    const cabe::Status s = cabe::Status::Corruption();
    EXPECT_TRUE(s.IsCorruption());
    EXPECT_EQ(s.ToString(), "Corruption");
}

TEST(StatusTest, ResourceExhausted) {
    const cabe::Status s = cabe::Status::ResourceExhausted();
    EXPECT_TRUE(s.IsResourceExhausted());
}

TEST(StatusTest, NotSupported) {
    const cabe::Status s = cabe::Status::NotSupported();
    EXPECT_TRUE(s.IsNotSupported());
}

TEST(StatusTest, OKToStringIsOK) {
    EXPECT_EQ(cabe::Status::OK().ToString(), "OK");
}

// ============================================================
// Engine 集成测试 fixture
// ============================================================

// 探测 O_DIRECT 支持：创建临时文件测试，测试后始终删除，不留残留。
// SupportsDirectIO 结束后文件被 unlink，Engine::Open（create_if_missing=true）
// 将为每个测试重新创建该文件。
static bool SupportsDirectIO(const char* path) {
    const int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    if (::ftruncate(fd, 64 * 1024 * 1024) < 0) {
        ::close(fd);
        ::unlink(path);
        return false;
    }
    ::close(fd);

    const int fd2 = ::open(path, O_RDWR | O_DIRECT | O_SYNC);
    ::unlink(path); // 无论成功与否都删除，由 Engine::Open 负责重建
    if (fd2 < 0) {
        return false;
    }
    ::close(fd2);
    return true;
}

class EngineApiTest : public ::testing::Test {
protected:
    std::unique_ptr<cabe::Engine> engine_;
    std::string devicePath_;

    void SetUp() override {
        const auto* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        devicePath_ = std::string("/var/tmp/cabe_api_")
                    + info->test_suite_name() + "_" + info->name()
                    + "_" + std::to_string(::getpid()) + ".dat";

        if (!SupportsDirectIO(devicePath_.c_str())) {
            GTEST_SKIP() << "O_DIRECT not supported at " << devicePath_;
        }
        // SupportsDirectIO 已删除探测文件；各测试按需调用 OpenDefault() 打开引擎
    }

    void TearDown() override {
        if (engine_ && engine_->IsOpen()) {
            (void)engine_->Close();
        }
        engine_.reset();
        ::unlink(devicePath_.c_str());
    }

    // 默认 Options：create_if_missing=true，64 MiB 预分配
    [[nodiscard]] cabe::Options DefaultOptions() const {
        cabe::Options opts;
        opts.path              = devicePath_;
        opts.create_if_missing = true;
        opts.error_if_exists   = false;
        opts.buffer_pool_count = 8;
        opts.initial_file_size = 64ULL * 1024 * 1024;
        return opts;
    }

    // 便捷打开：用默认 Options
    [[nodiscard]] cabe::Status OpenDefault() {
        return cabe::Engine::Open(DefaultOptions(), &engine_);
    }

    // 构造 N 字节的 value，全部填充 fill
    static std::vector<std::byte> MakeData(const size_t size,
                                           const uint8_t fill = 0x41) {
        return std::vector<std::byte>(size, static_cast<std::byte>(fill));
    }
};

// ============================================================
// Open / Close / IsOpen
// ============================================================

TEST_F(EngineApiTest, OpenAndClose) {
    ASSERT_TRUE(OpenDefault().ok());
    EXPECT_TRUE(engine_->IsOpen());

    EXPECT_TRUE(engine_->Close().ok());
    EXPECT_FALSE(engine_->IsOpen());
}

TEST_F(EngineApiTest, OpenNullResultReturnsInvalidArgument) {
    const cabe::Status s = cabe::Engine::Open(DefaultOptions(), nullptr);
    EXPECT_TRUE(s.IsInvalidArgument());
}

TEST_F(EngineApiTest, OpenEmptyPathReturnsInvalidArgument) {
    cabe::Options opts = DefaultOptions();
    opts.path = "";
    std::unique_ptr<cabe::Engine> e;
    const cabe::Status s = cabe::Engine::Open(opts, &e);
    EXPECT_TRUE(s.IsInvalidArgument());
    EXPECT_EQ(e, nullptr);
}

TEST_F(EngineApiTest, OpenZeroBufferPoolCountReturnsInvalidArgument) {
    cabe::Options opts = DefaultOptions();
    opts.buffer_pool_count = 0;
    std::unique_ptr<cabe::Engine> e;
    const cabe::Status s = cabe::Engine::Open(opts, &e);
    EXPECT_TRUE(s.IsInvalidArgument());
    EXPECT_EQ(e, nullptr);
}

TEST_F(EngineApiTest, OpenCreateIfMissingFalseFileNotExist) {
    // 文件不存在且 create_if_missing=false → NotFound
    cabe::Options opts = DefaultOptions();
    opts.create_if_missing = false;
    std::unique_ptr<cabe::Engine> e;
    const cabe::Status s = cabe::Engine::Open(opts, &e);
    EXPECT_TRUE(s.IsNotFound()) << s.ToString();
    EXPECT_EQ(e, nullptr);
}

TEST_F(EngineApiTest, OpenErrorIfExistsWhenFileAlreadyExists) {
    // 先创建文件
    ASSERT_TRUE(OpenDefault().ok());
    ASSERT_TRUE(engine_->Close().ok());
    engine_.reset();

    // error_if_exists=true 且文件已存在 → InvalidArgument
    cabe::Options opts = DefaultOptions();
    opts.error_if_exists = true;
    std::unique_ptr<cabe::Engine> e;
    const cabe::Status s = cabe::Engine::Open(opts, &e);
    EXPECT_TRUE(s.IsInvalidArgument()) << s.ToString();
    EXPECT_EQ(e, nullptr);
}

TEST_F(EngineApiTest, OpenSamePathTwiceIsIdempotent) {
    ASSERT_TRUE(OpenDefault().ok());

    // 已打开的 Engine 实例调用 Close 后可重新打开
    ASSERT_TRUE(engine_->Close().ok());
    engine_.reset();
    ASSERT_TRUE(OpenDefault().ok());
    EXPECT_TRUE(engine_->IsOpen());
}

TEST_F(EngineApiTest, DestructorClosesAutomatically) {
    ASSERT_TRUE(OpenDefault().ok());
    engine_.reset(); // 触发析构，不应 crash
    // TearDown 会再次 unlink，幂等
}

// ============================================================
// Put / Get 基本语义
// ============================================================

TEST_F(EngineApiTest, PutAndGetRoundTrip) {
    ASSERT_TRUE(OpenDefault().ok());

    const std::string key = "hello";
    const auto value      = MakeData(CABE_VALUE_DATA_SIZE, 0x42); // 1 MiB
    ASSERT_TRUE(engine_->Put({}, key, value).ok());

    std::vector<std::byte> out;
    ASSERT_TRUE(engine_->Get({}, key, &out).ok());
    ASSERT_EQ(out.size(), value.size());
    EXPECT_EQ(out, value);
}

TEST_F(EngineApiTest, PutEmptyKeyReturnsInvalidArgument) {
    ASSERT_TRUE(OpenDefault().ok());
    const auto value = MakeData(CABE_VALUE_DATA_SIZE);
    EXPECT_TRUE(engine_->Put({}, "", value).IsInvalidArgument());
}

TEST_F(EngineApiTest, PutEmptyValueReturnsInvalidArgument) {
    ASSERT_TRUE(OpenDefault().ok());
    EXPECT_TRUE(engine_->Put({}, "key", std::span<const std::byte>{}).IsInvalidArgument());
}

TEST_F(EngineApiTest, GetNotFoundReturnsNotFound) {
    ASSERT_TRUE(OpenDefault().ok());
    std::vector<std::byte> out;
    EXPECT_TRUE(engine_->Get({}, "nonexistent", &out).IsNotFound());
}

TEST_F(EngineApiTest, GetNullValuePtrReturnsInvalidArgument) {
    ASSERT_TRUE(OpenDefault().ok());
    EXPECT_TRUE(engine_->Get({}, "key", nullptr).IsInvalidArgument());
}

TEST_F(EngineApiTest, GetEmptyKeyReturnsInvalidArgument) {
    ASSERT_TRUE(OpenDefault().ok());
    std::vector<std::byte> out;
    EXPECT_TRUE(engine_->Get({}, "", &out).IsInvalidArgument());
}

TEST_F(EngineApiTest, GetPartialChunkValue) {
    // value 小于 1 MiB（单 chunk，有效载荷 < CABE_VALUE_DATA_SIZE）
    ASSERT_TRUE(OpenDefault().ok());

    const auto value = MakeData(512 * 1024, 0xCD); // 512 KiB
    ASSERT_TRUE(engine_->Put({}, "half", value).ok());

    std::vector<std::byte> out;
    ASSERT_TRUE(engine_->Get({}, "half", &out).ok());
    EXPECT_EQ(out, value);
}

// ============================================================
// 多 chunk 读写（value > 1 MiB）
// ============================================================

TEST_F(EngineApiTest, PutGetMultiChunk) {
    ASSERT_TRUE(OpenDefault().ok());

    // 3 MiB + 256 KiB（跨 3 个完整 chunk + 1 个尾 chunk）
    const size_t totalSize = 3 * CABE_VALUE_DATA_SIZE + 256 * 1024;
    std::vector<std::byte> value(totalSize);
    for (size_t i = 0; i < totalSize; ++i) {
        value[i] = static_cast<std::byte>(i & 0xFF);
    }

    ASSERT_TRUE(engine_->Put({}, "big", value).ok());

    std::vector<std::byte> out;
    ASSERT_TRUE(engine_->Get({}, "big", &out).ok());
    ASSERT_EQ(out.size(), totalSize);
    EXPECT_EQ(out, value);
}

// ============================================================
// Delete 语义
// ============================================================

TEST_F(EngineApiTest, DeleteBasic) {
    ASSERT_TRUE(OpenDefault().ok());

    const auto value = MakeData(CABE_VALUE_DATA_SIZE);
    ASSERT_TRUE(engine_->Put({}, "key", value).ok());
    ASSERT_TRUE(engine_->Delete({}, "key").ok());

    // Delete 后 Get 应返回 NotFound
    std::vector<std::byte> out;
    EXPECT_TRUE(engine_->Get({}, "key", &out).IsNotFound());
}

TEST_F(EngineApiTest, DeleteNotFoundReturnsNotFound) {
    ASSERT_TRUE(OpenDefault().ok());
    EXPECT_TRUE(engine_->Delete({}, "nonexistent").IsNotFound());
}

TEST_F(EngineApiTest, DeleteEmptyKeyReturnsInvalidArgument) {
    ASSERT_TRUE(OpenDefault().ok());
    EXPECT_TRUE(engine_->Delete({}, "").IsInvalidArgument());
}

TEST_F(EngineApiTest, DeleteAndRePutSameKey) {
    // Delete 后应当可以重新写入相同 key（磁盘块已回收）
    ASSERT_TRUE(OpenDefault().ok());

    const auto v1 = MakeData(CABE_VALUE_DATA_SIZE, 0xAA);
    ASSERT_TRUE(engine_->Put({}, "reuse", v1).ok());
    ASSERT_TRUE(engine_->Delete({}, "reuse").ok());

    const auto v2 = MakeData(CABE_VALUE_DATA_SIZE, 0xBB);
    ASSERT_TRUE(engine_->Put({}, "reuse", v2).ok());

    std::vector<std::byte> out;
    ASSERT_TRUE(engine_->Get({}, "reuse", &out).ok());
    EXPECT_EQ(out, v2);
}

// ============================================================
// 覆盖写（overwrite）
// ============================================================

TEST_F(EngineApiTest, PutOverwriteNewDataReadable) {
    ASSERT_TRUE(OpenDefault().ok());

    const auto v1 = MakeData(CABE_VALUE_DATA_SIZE, 0x11);
    const auto v2 = MakeData(CABE_VALUE_DATA_SIZE, 0x22);

    ASSERT_TRUE(engine_->Put({}, "k", v1).ok());
    ASSERT_TRUE(engine_->Put({}, "k", v2).ok()); // 覆盖写

    std::vector<std::byte> out;
    ASSERT_TRUE(engine_->Get({}, "k", &out).ok());
    EXPECT_EQ(out, v2); // 新值可读
}

TEST_F(EngineApiTest, PutOverwriteDoesNotChangeSize) {
    // 覆盖写同一 key，Size() 不变
    ASSERT_TRUE(OpenDefault().ok());
    EXPECT_EQ(engine_->Size(), 0u);

    const auto v = MakeData(CABE_VALUE_DATA_SIZE);
    ASSERT_TRUE(engine_->Put({}, "k", v).ok());
    EXPECT_EQ(engine_->Size(), 1u);

    ASSERT_TRUE(engine_->Put({}, "k", v).ok()); // 覆盖
    EXPECT_EQ(engine_->Size(), 1u);
}

// ============================================================
// Size() 计数语义
// ============================================================

TEST_F(EngineApiTest, SizeInitiallyZero) {
    ASSERT_TRUE(OpenDefault().ok());
    EXPECT_EQ(engine_->Size(), 0u);
}

TEST_F(EngineApiTest, SizeTracksInsertAndDelete) {
    ASSERT_TRUE(OpenDefault().ok());

    const auto v = MakeData(CABE_VALUE_DATA_SIZE);
    ASSERT_TRUE(engine_->Put({}, "a", v).ok());
    ASSERT_TRUE(engine_->Put({}, "b", v).ok());
    EXPECT_EQ(engine_->Size(), 2u);

    ASSERT_TRUE(engine_->Delete({}, "a").ok());
    EXPECT_EQ(engine_->Size(), 1u); // Delete 立即减少计数

    ASSERT_TRUE(engine_->Delete({}, "b").ok());
    EXPECT_EQ(engine_->Size(), 0u);
}

// ============================================================
// 多 key 独立性
// ============================================================

TEST_F(EngineApiTest, MultipleKeysIndependent) {
    ASSERT_TRUE(OpenDefault().ok());

    const auto va = MakeData(CABE_VALUE_DATA_SIZE, 0xAA);
    const auto vb = MakeData(CABE_VALUE_DATA_SIZE, 0xBB);
    const auto vc = MakeData(CABE_VALUE_DATA_SIZE, 0xCC);

    ASSERT_TRUE(engine_->Put({}, "a", va).ok());
    ASSERT_TRUE(engine_->Put({}, "b", vb).ok());
    ASSERT_TRUE(engine_->Put({}, "c", vc).ok());

    ASSERT_TRUE(engine_->Delete({}, "b").ok());

    // a 和 c 的数据不受影响
    std::vector<std::byte> out;
    ASSERT_TRUE(engine_->Get({}, "a", &out).ok());
    EXPECT_EQ(out, va);

    EXPECT_TRUE(engine_->Get({}, "b", &out).IsNotFound());

    ASSERT_TRUE(engine_->Get({}, "c", &out).ok());
    EXPECT_EQ(out, vc);
}
