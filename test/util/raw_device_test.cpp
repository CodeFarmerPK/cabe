#include "util/raw_device.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string GetTestDevice() {
    const char* dev = std::getenv("CABE_TEST_DEVICE");
    return dev ? std::string(dev) : "";
}

} // namespace

class RawDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = GetTestDevice();
        if (device_.empty()) GTEST_SKIP() << "CABE_TEST_DEVICE 未设置";
    }
    void TearDown() override { dev_.Close(); }
    cabe::RawDevice dev_;
    std::string device_;
};

TEST_F(RawDeviceTest, OpenCloseNormal) {
    EXPECT_FALSE(dev_.is_open());
    EXPECT_EQ(dev_.Open(device_), cabe::err::kSuccess);
    EXPECT_TRUE(dev_.is_open());
    EXPECT_EQ(dev_.Close(), cabe::err::kSuccess);
    EXPECT_FALSE(dev_.is_open());
}

TEST_F(RawDeviceTest, SizeBytesPositive) {
    ASSERT_EQ(dev_.Open(device_), cabe::err::kSuccess);
    EXPECT_GT(dev_.SizeBytes(), 0u);
}

TEST_F(RawDeviceTest, WriteReadAlignedRoundTrip) {
    ASSERT_EQ(dev_.Open(device_), cabe::err::kSuccess);
    constexpr std::size_t kLen = 4096;
    std::byte* wbuf = cabe::RawDevice::AllocAligned(kLen);
    std::byte* rbuf = cabe::RawDevice::AllocAligned(kLen);
    ASSERT_NE(wbuf, nullptr);
    ASSERT_NE(rbuf, nullptr);
    std::memset(wbuf, 0x5A, kLen);
    std::memset(rbuf, 0, kLen);

    EXPECT_EQ(dev_.WriteAt(0, wbuf, kLen), cabe::err::kSuccess);
    EXPECT_EQ(dev_.ReadAt(0, rbuf, kLen), cabe::err::kSuccess);
    EXPECT_EQ(std::memcmp(wbuf, rbuf, kLen), 0);

    cabe::RawDevice::FreeAligned(wbuf);
    cabe::RawDevice::FreeAligned(rbuf);
}

TEST_F(RawDeviceTest, AllocAlignedIs4KAligned) {
    std::byte* p = cabe::RawDevice::AllocAligned(8192);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % cabe::RawDevice::kAlignment, 0u);
    cabe::RawDevice::FreeAligned(p);
}

TEST(RawDeviceNoDevice, OpenBadPath) {
    cabe::RawDevice dev;
    EXPECT_NE(dev.Open("/no/such/device"), cabe::err::kSuccess);
}
