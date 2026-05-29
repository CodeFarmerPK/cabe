#include "engine/engine.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::string GetEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : "";
}

std::vector<std::byte> MakeValue(std::byte fill) {
    std::vector<std::byte> v(cabe::kValueSize);
    std::memset(v.data(), static_cast<int>(fill), cabe::kValueSize);
    return v;
}

} // namespace

// =========================================================================
// 需要 3 个设备的测试（P5M1：数据 + WAL + 快照）
// 环境变量：CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE
// 注：M1 阶段 WAL/快照仅写超级块，索引在单个 Open 会话内有效（重启恢复在 M5）。
// =========================================================================
class EngineDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_  = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            GTEST_SKIP() << "需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE";
        }
    }
    void TearDown() override { engine_.Close(); }

    // create 模式打开（写超级块 + 空索引）
    cabe::Options CreateOpts() {
        cabe::Options opts;
        opts.devices.push_back({data_, wal_, snap_});
        opts.create = true;
        return opts;
    }

    cabe::Engine engine_;
    std::string data_, wal_, snap_;
};

TEST_F(EngineDeviceTest, OpenCloseNormal) {
    EXPECT_FALSE(engine_.is_open());
    auto s = engine_.Open(CreateOpts());
    EXPECT_TRUE(s.ok()) << "code=" << s.code;
    EXPECT_TRUE(engine_.is_open());
    s = engine_.Close();
    EXPECT_TRUE(s.ok());
    EXPECT_FALSE(engine_.is_open());
}

TEST_F(EngineDeviceTest, DoubleOpenFails) {
    EXPECT_TRUE(engine_.Open(CreateOpts()).ok());
    EXPECT_EQ(engine_.Open(CreateOpts()).code, cabe::err::kEngineAlreadyOpen);
}

TEST_F(EngineDeviceTest, DestructorAutoCloses) {
    { cabe::Engine tmp; EXPECT_TRUE(tmp.Open(CreateOpts()).ok()); }
}

// ---- Put / Get 端到端（单会话，内存索引）----

TEST_F(EngineDeviceTest, PutGetRoundTrip) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    auto value = MakeValue(std::byte{0xAB});
    EXPECT_TRUE(engine_.Put("key1", cabe::DataView{value}).ok());

    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_TRUE(engine_.Get("key1", cabe::DataBuffer{out}).ok());
    EXPECT_EQ(std::memcmp(value.data(), out.data(), cabe::kValueSize), 0);
}

TEST_F(EngineDeviceTest, PutGetMultipleKeys) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    for (int i = 0; i < 3; ++i) {
        auto val = MakeValue(static_cast<std::byte>(0x10 + i));
        EXPECT_TRUE(engine_.Put("mk" + std::to_string(i), cabe::DataView{val}).ok());
    }
    for (int i = 0; i < 3; ++i) {
        std::vector<std::byte> out(cabe::kValueSize);
        EXPECT_TRUE(engine_.Get("mk" + std::to_string(i), cabe::DataBuffer{out}).ok());
        EXPECT_EQ(static_cast<unsigned char>(out[0]), 0x10 + i);
    }
}

TEST_F(EngineDeviceTest, PutOverwrite) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    EXPECT_TRUE(engine_.Put("ow", cabe::DataView{MakeValue(std::byte{0x11})}).ok());
    EXPECT_TRUE(engine_.Put("ow", cabe::DataView{MakeValue(std::byte{0x22})}).ok());

    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_TRUE(engine_.Get("ow", cabe::DataBuffer{out}).ok());
    EXPECT_EQ(out[0], std::byte{0x22});
}

// ---- Delete ----

TEST_F(EngineDeviceTest, DeleteThenGetFails) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    EXPECT_TRUE(engine_.Put("del1", cabe::DataView{MakeValue(std::byte{0xDD})}).ok());
    EXPECT_TRUE(engine_.Delete("del1").ok());

    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("del1", cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
}

TEST_F(EngineDeviceTest, DeleteNotFound) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    EXPECT_EQ(engine_.Delete("no-such").code, cabe::err::kIndexKeyNotFound);
}

TEST_F(EngineDeviceTest, GetNotFound) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("no-such", cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
}

// ---- 容量边界（注：逻辑 block 从 0 起，可用块数 = (设备字节 - kDataRegionOffset)/kValueSize）----

TEST_F(EngineDeviceTest, PutUntilFull) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    auto val = MakeValue(std::byte{0xFF});
    int count = 0;
    cabe::Status s;
    for (int i = 0; i < 200000; ++i) {
        s = engine_.Put("full" + std::to_string(i), cabe::DataView{val});
        if (!s.ok()) break;
        ++count;
    }
    EXPECT_EQ(s.code, cabe::err::kEngineNoSpace);
    EXPECT_GT(count, 0);
}

TEST_F(EngineDeviceTest, DeleteFreesThenPutAgain) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    auto val = MakeValue(std::byte{0xEE});
    int count = 0;
    for (int i = 0; i < 200000; ++i) {
        if (!engine_.Put("df" + std::to_string(i), cabe::DataView{val}).ok()) break;
        ++count;
    }
    ASSERT_GT(count, 0);
    EXPECT_TRUE(engine_.Delete("df0").ok());
    EXPECT_TRUE(engine_.Put("df-new", cabe::DataView{val}).ok());
}

// ---- CRC32 校验（注：逻辑 block 0 物理偏移 = kDataRegionOffset，头部 8K 为超级块）----

TEST_F(EngineDeviceTest, CRC32Verified) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    EXPECT_TRUE(engine_.Put("crc-key", cabe::DataView{MakeValue(std::byte{0xCC})}).ok());

    // 第一个 Put 写到 block 0，物理偏移 = kDataRegionOffset（头部 8K 超级块之后）
    int fd = ::open(data_.c_str(), O_RDWR | O_DIRECT, 0);
    ASSERT_GE(fd, 0);
    void* raw = nullptr;
    ASSERT_EQ(posix_memalign(&raw, 4096, cabe::kValueSize), 0);
    auto* tamper = static_cast<std::byte*>(raw);
    std::memset(tamper, 0xCC, cabe::kValueSize);
    tamper[0] = std::byte{0x00};
    ::pwrite(fd, tamper, cabe::kValueSize, static_cast<off_t>(cabe::kDataRegionOffset));  // block 0
    ::close(fd);
    std::free(raw);

    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("crc-key", cabe::DataBuffer{out}).code, cabe::err::kEngineDataCorrupted);
}

// ---- 前置校验 ----

TEST_F(EngineDeviceTest, PutEmptyKeyFails) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    EXPECT_EQ(engine_.Put("", cabe::DataView{MakeValue(std::byte{0})}).code, cabe::err::kMemEmptyKey);
}

TEST_F(EngineDeviceTest, PutWrongValueSizeFails) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    std::vector<std::byte> small(100);
    EXPECT_EQ(engine_.Put("k", cabe::DataView{small}).code, cabe::err::kEngineInvalidValue);
}

// =========================================================================
// 不需要设备的测试
// =========================================================================

TEST(Engine, CloseWithoutOpenFails) {
    cabe::Engine e;
    EXPECT_EQ(e.Close().code, cabe::err::kEngineNotOpen);
}

TEST(Engine, PutWithoutOpenFails) {
    cabe::Engine e;
    std::vector<std::byte> buf(cabe::kValueSize);
    EXPECT_EQ(e.Put("key", cabe::DataView{buf}).code, cabe::err::kEngineNotOpen);
}

TEST(Engine, EmptyDevicesFails) {
    cabe::Engine e;
    EXPECT_EQ(e.Open(cabe::Options{}).code, cabe::err::kEngineInvalidOpts);
}

TEST(Engine, MultipleDevicesFails) {
    cabe::Engine e;
    cabe::Options opts;
    opts.devices.push_back({"d1", "w1", "s1"});
    opts.devices.push_back({"d2", "w2", "s2"});
    EXPECT_EQ(e.Open(opts).code, cabe::err::kEngineInvalidOpts);
}
