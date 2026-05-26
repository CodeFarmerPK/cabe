#include "io/sync/sync_io_backend.h"
#include "engine/buffer_pool.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string GetTestDevice() {
    const char* dev = std::getenv("CABE_TEST_DEVICE");
    return dev ? std::string(dev) : "";
}

} // namespace

// 需要设备的测试
class SyncIoBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = GetTestDevice();
        if (device_.empty()) GTEST_SKIP() << "CABE_TEST_DEVICE 未设置";
    }
    void TearDown() override {
        backend_.Close();
    }
    cabe::SyncIoBackend backend_;
    std::string device_;
};

TEST_F(SyncIoBackendTest, OpenCloseNormal) {
    EXPECT_FALSE(backend_.is_open());
    EXPECT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    EXPECT_TRUE(backend_.is_open());
    EXPECT_EQ(backend_.Close(), cabe::err::kSuccess);
    EXPECT_FALSE(backend_.is_open());
}

TEST_F(SyncIoBackendTest, DoubleOpenFails) {
    EXPECT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    EXPECT_NE(backend_.Open(device_), cabe::err::kSuccess);
}

TEST_F(SyncIoBackendTest, CloseIdempotent) {
    EXPECT_EQ(backend_.Close(), cabe::err::kSuccess);
}

TEST_F(SyncIoBackendTest, BlockCountCorrect) {
    EXPECT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    EXPECT_GT(backend_.BlockCount(), 0u);
}

TEST_F(SyncIoBackendTest, WriteReadRoundTrip) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);

    cabe::BufferPool pool(2);
    auto* wbuf = pool.Allocate();
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0xAB, cabe::kValueSize);

    EXPECT_EQ(backend_.Write(0, wbuf), cabe::err::kSuccess);
    pool.Free(wbuf);

    auto* rbuf = pool.Allocate();
    ASSERT_NE(rbuf, nullptr);
    std::memset(rbuf, 0, cabe::kValueSize);

    EXPECT_EQ(backend_.Read(0, rbuf), cabe::err::kSuccess);
    EXPECT_EQ(std::memcmp(wbuf, rbuf, cabe::kValueSize), 0);
    pool.Free(rbuf);
}

TEST_F(SyncIoBackendTest, WriteReadMultipleBlocks) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    cabe::BufferPool pool(4);

    for (std::uint64_t i = 0; i < 4; ++i) {
        auto* wbuf = pool.Allocate();
        ASSERT_NE(wbuf, nullptr);
        std::memset(wbuf, static_cast<int>(0x10 + i), cabe::kValueSize);
        EXPECT_EQ(backend_.Write(i, wbuf), cabe::err::kSuccess);
        pool.Free(wbuf);
    }

    for (std::uint64_t i = 0; i < 4; ++i) {
        auto* rbuf = pool.Allocate();
        ASSERT_NE(rbuf, nullptr);
        EXPECT_EQ(backend_.Read(i, rbuf), cabe::err::kSuccess);
        EXPECT_EQ(static_cast<unsigned char>(rbuf[0]), 0x10 + i);
        pool.Free(rbuf);
    }
}

TEST_F(SyncIoBackendTest, DestructorAutoCloses) {
    {
        cabe::SyncIoBackend tmp;
        EXPECT_EQ(tmp.Open(device_), cabe::err::kSuccess);
    }
}

TEST_F(SyncIoBackendTest, MoveConstruct) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    auto count = backend_.BlockCount();

    cabe::SyncIoBackend moved(std::move(backend_));
    EXPECT_TRUE(moved.is_open());
    EXPECT_EQ(moved.BlockCount(), count);
    EXPECT_FALSE(backend_.is_open());
    moved.Close();
}

TEST_F(SyncIoBackendTest, ConceptSatisfied) {
    static_assert(cabe::IoBackend<cabe::SyncIoBackend>);
}

// 不需要设备的测试
TEST(SyncIoBackendNoDevice, OpenBadPath) {
    cabe::SyncIoBackend backend;
    EXPECT_NE(backend.Open("/no/such/device"), cabe::err::kSuccess);
}
