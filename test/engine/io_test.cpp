#include "engine/io.h"
#include "engine/buffer_pool.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

std::string GetTestDevice() {
    const char* dev = std::getenv("CABE_TEST_DEVICE");
    return dev ? std::string(dev) : "";
}

class IoTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = GetTestDevice();
        if (device_.empty()) {
            GTEST_SKIP() << "CABE_TEST_DEVICE 未设置";
        }
        fd_ = ::open(device_.c_str(), O_RDWR | O_DIRECT, 0);
        ASSERT_GE(fd_, 0) << "无法打开 " << device_;
        pool_ = cabe::BufferPool(4);
    }

    void TearDown() override {
        if (fd_ >= 0) ::close(fd_);
    }

    std::string device_;
    int fd_ = -1;
    cabe::BufferPool pool_{0};
};

} // namespace

TEST_F(IoTest, WriteReadRoundTrip) {
    auto* wbuf = pool_.Allocate();
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0xAB, cabe::kValueSize);

    int32_t rc = cabe::WriteBlock(fd_, 0, wbuf);
    EXPECT_EQ(rc, cabe::err::kSuccess) << "WriteBlock 失败: rc=" << rc;
    pool_.Free(wbuf);

    auto* rbuf = pool_.Allocate();
    ASSERT_NE(rbuf, nullptr);
    std::memset(rbuf, 0, cabe::kValueSize);

    rc = cabe::ReadBlock(fd_, 0, rbuf);
    EXPECT_EQ(rc, cabe::err::kSuccess) << "ReadBlock 失败: rc=" << rc;

    EXPECT_EQ(std::memcmp(wbuf, rbuf, cabe::kValueSize), 0)
        << "写入和读回的数据不一致";
    pool_.Free(rbuf);
}

TEST_F(IoTest, WriteReadMultipleBlocks) {
    for (std::uint64_t i = 0; i < 4; ++i) {
        auto* wbuf = pool_.Allocate();
        ASSERT_NE(wbuf, nullptr);
        std::memset(wbuf, static_cast<int>(0x10 + i), cabe::kValueSize);
        EXPECT_EQ(cabe::WriteBlock(fd_, i, wbuf), cabe::err::kSuccess);
        pool_.Free(wbuf);
    }

    for (std::uint64_t i = 0; i < 4; ++i) {
        auto* rbuf = pool_.Allocate();
        ASSERT_NE(rbuf, nullptr);
        EXPECT_EQ(cabe::ReadBlock(fd_, i, rbuf), cabe::err::kSuccess);
        EXPECT_EQ(static_cast<unsigned char>(rbuf[0]), 0x10 + i)
            << "block " << i << " 数据不一致";
        pool_.Free(rbuf);
    }
}

TEST_F(IoTest, ReadUnwrittenBlock) {
    auto* rbuf = pool_.Allocate();
    ASSERT_NE(rbuf, nullptr);
    std::memset(rbuf, 0xFF, cabe::kValueSize);

    // 读一个在设备范围内但从未写过的块（WriteReadMultipleBlocks 只写了 0-3）
    int32_t rc = cabe::ReadBlock(fd_, 50, rbuf);
    EXPECT_EQ(rc, cabe::err::kSuccess);

    bool all_zero = true;
    for (std::size_t j = 0; j < cabe::kValueSize; ++j) {
        if (rbuf[j] != std::byte{0}) { all_zero = false; break; }
    }
    EXPECT_TRUE(all_zero) << "未写过的块应全零（块设备特性）";
    pool_.Free(rbuf);
}
