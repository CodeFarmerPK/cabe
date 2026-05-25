#include "engine/engine.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

std::string TempPath() {
    return std::filesystem::temp_directory_path() / "cabe-engine-test.data";
}

cabe::Options SingleDeviceOpts() {
    return cabe::Options{{cabe::DeviceConfig{TempPath()}}};
}

class EngineTest : public ::testing::Test {
protected:
    void TearDown() override {
        std::filesystem::remove(TempPath());
    }
};

} // namespace

TEST_F(EngineTest, OpenCloseNormal) {
    cabe::Engine engine;
    EXPECT_FALSE(engine.is_open());

    auto s = engine.Open(SingleDeviceOpts());
    EXPECT_TRUE(s.ok()) << "code=" << s.code;
    EXPECT_TRUE(engine.is_open());

    s = engine.Close();
    EXPECT_TRUE(s.ok());
    EXPECT_FALSE(engine.is_open());
}

TEST_F(EngineTest, DoubleOpenFails) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(SingleDeviceOpts()).ok());
    auto s = engine.Open(SingleDeviceOpts());
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, cabe::err::kEngineAlreadyOpen);
    engine.Close();
}

TEST_F(EngineTest, CloseWithoutOpenFails) {
    cabe::Engine engine;
    auto s = engine.Close();
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, cabe::err::kEngineNotOpen);
}

TEST_F(EngineTest, DestructorAutoCloses) {
    {
        cabe::Engine engine;
        EXPECT_TRUE(engine.Open(SingleDeviceOpts()).ok());
    }
}

TEST_F(EngineTest, EmptyDevicesFails) {
    cabe::Engine engine;
    cabe::Options opts;
    auto s = engine.Open(opts);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, cabe::err::kEngineInvalidOpts);
}

TEST_F(EngineTest, MultipleDevicesFails) {
    cabe::Engine engine;
    cabe::Options opts{{
        cabe::DeviceConfig{TempPath()},
        cabe::DeviceConfig{TempPath() + ".2"},
    }};
    auto s = engine.Open(opts);
    EXPECT_FALSE(s.ok());
    EXPECT_EQ(s.code, cabe::err::kEngineInvalidOpts);
}

TEST_F(EngineTest, PutWithoutOpenFails) {
    cabe::Engine engine;
    std::vector<std::byte> buf(cabe::kValueSize);
    auto s = engine.Put("key", cabe::DataView{buf});
    EXPECT_EQ(s.code, cabe::err::kEngineNotOpen);
}

TEST_F(EngineTest, PutEmptyKeyFails) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(SingleDeviceOpts()).ok());
    std::vector<std::byte> buf(cabe::kValueSize);
    auto s = engine.Put("", cabe::DataView{buf});
    EXPECT_EQ(s.code, cabe::err::kMemEmptyKey);
    engine.Close();
}

TEST_F(EngineTest, PutWrongValueSizeFails) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(SingleDeviceOpts()).ok());
    std::vector<std::byte> buf(100);
    auto s = engine.Put("key", cabe::DataView{buf});
    EXPECT_EQ(s.code, cabe::err::kEngineInvalidValue);
    engine.Close();
}

TEST_F(EngineTest, PutReturnsNotImplemented) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(SingleDeviceOpts()).ok());
    std::vector<std::byte> buf(cabe::kValueSize);
    auto s = engine.Put("key", cabe::DataView{buf});
    EXPECT_EQ(s.code, cabe::err::kEngineNotImplemented);
    engine.Close();
}

TEST_F(EngineTest, GetReturnsNotImplemented) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(SingleDeviceOpts()).ok());
    std::vector<std::byte> buf(cabe::kValueSize);
    auto s = engine.Get("key", cabe::DataBuffer{buf});
    EXPECT_EQ(s.code, cabe::err::kEngineNotImplemented);
    engine.Close();
}

TEST_F(EngineTest, DeleteReturnsNotImplemented) {
    cabe::Engine engine;
    EXPECT_TRUE(engine.Open(SingleDeviceOpts()).ok());
    auto s = engine.Delete("key");
    EXPECT_EQ(s.code, cabe::err::kEngineNotImplemented);
    engine.Close();
}
