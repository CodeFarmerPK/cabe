#include "engine/engine.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::string GetTestDevice() {
    const char* dev = std::getenv("CABE_TEST_DEVICE");
    return dev ? std::string(dev) : "";
}

cabe::Options MakeOpts(const std::string& path) {
    return cabe::Options{{cabe::DeviceConfig{path}}};
}

std::vector<std::byte> MakeValue(std::byte fill) {
    std::vector<std::byte> v(cabe::kValueSize);
    std::memset(v.data(), static_cast<int>(fill), cabe::kValueSize);
    return v;
}

} // namespace

// =========================================================================
// 需要设备的测试
// =========================================================================
class EngineDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = GetTestDevice();
        if (device_.empty()) GTEST_SKIP() << "CABE_TEST_DEVICE 未设置";
    }
    void TearDown() override { engine_.Close(); }
    cabe::Engine engine_;
    std::string device_;
};

TEST_F(EngineDeviceTest, OpenCloseNormal) {
    EXPECT_FALSE(engine_.is_open());
    auto s = engine_.Open(MakeOpts(device_));
    EXPECT_TRUE(s.ok()) << "code=" << s.code;
    EXPECT_TRUE(engine_.is_open());
    s = engine_.Close();
    EXPECT_TRUE(s.ok());
    EXPECT_FALSE(engine_.is_open());
}

TEST_F(EngineDeviceTest, DoubleOpenFails) {
    EXPECT_TRUE(engine_.Open(MakeOpts(device_)).ok());
    EXPECT_EQ(engine_.Open(MakeOpts(device_)).code, cabe::err::kEngineAlreadyOpen);
}

TEST_F(EngineDeviceTest, DestructorAutoCloses) {
    { cabe::Engine tmp; EXPECT_TRUE(tmp.Open(MakeOpts(device_)).ok()); }
}

// ---- Put / Get 端到端 ----

TEST_F(EngineDeviceTest, PutGetRoundTrip) {
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
    auto value = MakeValue(std::byte{0xAB});
    EXPECT_TRUE(engine_.Put("key1", cabe::DataView{value}).ok());

    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_TRUE(engine_.Get("key1", cabe::DataBuffer{out}).ok());
    EXPECT_EQ(std::memcmp(value.data(), out.data(), cabe::kValueSize), 0);
}

TEST_F(EngineDeviceTest, PutGetMultipleKeys) {
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
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
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
    EXPECT_TRUE(engine_.Put("ow", cabe::DataView{MakeValue(std::byte{0x11})}).ok());
    EXPECT_TRUE(engine_.Put("ow", cabe::DataView{MakeValue(std::byte{0x22})}).ok());

    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_TRUE(engine_.Get("ow", cabe::DataBuffer{out}).ok());
    EXPECT_EQ(out[0], std::byte{0x22});
}

// ---- Delete ----

TEST_F(EngineDeviceTest, DeleteThenGetFails) {
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
    EXPECT_TRUE(engine_.Put("del1", cabe::DataView{MakeValue(std::byte{0xDD})}).ok());
    EXPECT_TRUE(engine_.Delete("del1").ok());

    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("del1", cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
}

TEST_F(EngineDeviceTest, DeleteNotFound) {
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
    EXPECT_EQ(engine_.Delete("no-such").code, cabe::err::kIndexKeyNotFound);
}

TEST_F(EngineDeviceTest, GetNotFound) {
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("no-such", cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
}

// ---- 容量边界 ----

TEST_F(EngineDeviceTest, PutUntilFull) {
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
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
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
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

// ---- CRC32 校验 ----

TEST_F(EngineDeviceTest, CRC32Verified) {
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
    EXPECT_TRUE(engine_.Put("crc-key", cabe::DataView{MakeValue(std::byte{0xCC})}).ok());

    // 手动篡改设备上 block 0 的第一个字节
    int fd = ::open(device_.c_str(), O_RDWR | O_DIRECT, 0);
    ASSERT_GE(fd, 0);
    void* raw = nullptr;
    ASSERT_EQ(posix_memalign(&raw, 4096, cabe::kValueSize), 0);
    auto* tamper = static_cast<std::byte*>(raw);
    std::memset(tamper, 0xCC, cabe::kValueSize);
    tamper[0] = std::byte{0x00};
    ::pwrite(fd, tamper, cabe::kValueSize, 0);
    ::close(fd);
    std::free(raw);

    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("crc-key", cabe::DataBuffer{out}).code, cabe::err::kEngineDataCorrupted);
}

// ---- 前置校验 ----

TEST_F(EngineDeviceTest, PutEmptyKeyFails) {
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
    EXPECT_EQ(engine_.Put("", cabe::DataView{MakeValue(std::byte{0})}).code, cabe::err::kMemEmptyKey);
}

TEST_F(EngineDeviceTest, PutWrongValueSizeFails) {
    ASSERT_TRUE(engine_.Open(MakeOpts(device_)).ok());
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
    cabe::Options opts{{cabe::DeviceConfig{"d1"}, cabe::DeviceConfig{"d2"}}};
    EXPECT_EQ(e.Open(opts).code, cabe::err::kEngineInvalidOpts);
}
