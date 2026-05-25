#include "engine/engine.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

std::string GetTestDevice() {
    const char* dev = std::getenv("CABE_TEST_DEVICE");
    return dev ? std::string(dev) : "";
}

cabe::Options MakeOpts(const std::string& path) {
    return cabe::Options{{cabe::DeviceConfig{path}}};
}

} // namespace

// 需要设备的测试（CABE_TEST_DEVICE 未设置时 SKIP）
class EngineDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = GetTestDevice();
        if (device_.empty()) GTEST_SKIP() << "CABE_TEST_DEVICE 未设置";
    }
    std::string device_;
};

TEST_F(EngineDeviceTest, OpenCloseNormal) {
    cabe::Engine engine;
    EXPECT_FALSE(engine.is_open());

    auto s = engine.Open(MakeOpts(device_));
    EXPECT_TRUE(s.ok()) << "code=" << s.code;
    EXPECT_TRUE(engine.is_open());

    s = engine.Close();
    EXPECT_TRUE(s.ok());
    EXPECT_FALSE(engine.is_open());
}

TEST_F(EngineDeviceTest, DoubleOpenFails) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(MakeOpts(device_)).ok());
    auto s = engine.Open(MakeOpts(device_));
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, cabe::err::kEngineAlreadyOpen);
    engine.Close();
}

TEST_F(EngineDeviceTest, DestructorAutoCloses) {
    {
        cabe::Engine engine;
        EXPECT_TRUE(engine.Open(MakeOpts(device_)).ok());
    }
}

TEST_F(EngineDeviceTest, PutEmptyKeyFails) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(MakeOpts(device_)).ok());
    std::vector<std::byte> buf(cabe::kValueSize);
    auto s = engine.Put("", cabe::DataView{buf});
    EXPECT_EQ(s.code, cabe::err::kMemEmptyKey);
    engine.Close();
}

TEST_F(EngineDeviceTest, PutWrongValueSizeFails) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(MakeOpts(device_)).ok());
    std::vector<std::byte> buf(100);
    auto s = engine.Put("key", cabe::DataView{buf});
    EXPECT_EQ(s.code, cabe::err::kEngineInvalidValue);
    engine.Close();
}

TEST_F(EngineDeviceTest, PutReturnsNotImplemented) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(MakeOpts(device_)).ok());
    std::vector<std::byte> buf(cabe::kValueSize);
    auto s = engine.Put("key", cabe::DataView{buf});
    EXPECT_EQ(s.code, cabe::err::kEngineNotImplemented);
    engine.Close();
}

TEST_F(EngineDeviceTest, GetReturnsNotImplemented) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(MakeOpts(device_)).ok());
    std::vector<std::byte> buf(cabe::kValueSize);
    auto s = engine.Get("key", cabe::DataBuffer{buf});
    EXPECT_EQ(s.code, cabe::err::kEngineNotImplemented);
    engine.Close();
}

TEST_F(EngineDeviceTest, DeleteReturnsNotImplemented) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(MakeOpts(device_)).ok());
    auto s = engine.Delete("key");
    EXPECT_EQ(s.code, cabe::err::kEngineNotImplemented);
    engine.Close();
}

// 不需要设备的测试（纯状态检查，不涉及 Open）
TEST(Engine, CloseWithoutOpenFails) {
    cabe::Engine engine;
    auto s = engine.Close();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, cabe::err::kEngineNotOpen);
}

TEST(Engine, PutWithoutOpenFails) {
    cabe::Engine engine;
    std::vector<std::byte> buf(cabe::kValueSize);
    auto s = engine.Put("key", cabe::DataView{buf});
    EXPECT_EQ(s.code, cabe::err::kEngineNotOpen);
}

TEST(Engine, EmptyDevicesFails) {
    cabe::Engine engine;
    cabe::Options opts;
    auto s = engine.Open(opts);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, cabe::err::kEngineInvalidOpts);
}

TEST(Engine, MultipleDevicesFails) {
    cabe::Engine engine;
    cabe::Options opts{{
        cabe::DeviceConfig{"dev1"},
        cabe::DeviceConfig{"dev2"},
    }};
    auto s = engine.Open(opts);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, cabe::err::kEngineInvalidOpts);
}
