#include "engine/super_block.h"
#include "engine/options.h"
#include "common/error_code.h"
#include "util/raw_device.h"
#include "test/common/test_env.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace {

using cabe::test::GetEnv;   // P5M4：收敛到共享测试头（原各文件逐字拷贝）

} // namespace

// 需要 3 个 loop 设备：CABE_TEST_DEVICE（数据）+ CABE_TEST_WAL_DEVICE + CABE_TEST_SNAPSHOT_DEVICE
class SuperBlockTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_  = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            GTEST_SKIP() << "需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE";
        }
        cfg_.data_path = data_;
        cfg_.wal_path = wal_;
        cfg_.snapshot_path = snap_;
    }
    cabe::DeviceConfig cfg_;
    std::string data_, wal_, snap_;
};

TEST_F(SuperBlockTest, CreateThenRecover) {
    cabe::SuperBlock sb{};
    ASSERT_EQ(cabe::CreateDeviceGroup(cfg_, 0, 1, &sb), cabe::err::kSuccess);
    EXPECT_EQ(sb.magic, cabe::kSuperBlockMagic);
    EXPECT_GT(sb.block_count, 0u);

    cabe::SuperBlock sb2{};
    EXPECT_EQ(cabe::RecoverDeviceGroup(cfg_, 0, 1, &sb2), cabe::err::kSuccess);
    EXPECT_EQ(std::memcmp(sb.device_uuid, sb2.device_uuid, cabe::kUuidBytes), 0);
    EXPECT_EQ(std::memcmp(sb.engine_uuid, sb2.engine_uuid, cabe::kUuidBytes), 0);
    // 往返一致：block_count / device_id / magic 应原样恢复（block_count 喂给 BlockAllocator）
    EXPECT_EQ(sb2.block_count, sb.block_count);
    EXPECT_EQ(sb2.device_id, 0u);
    EXPECT_EQ(sb2.magic, cabe::kSuperBlockMagic);
}

TEST_F(SuperBlockTest, RecoverDeviceIdMismatch) {
    cabe::SuperBlock sb{};
    ASSERT_EQ(cabe::CreateDeviceGroup(cfg_, 0, 1, &sb), cabe::err::kSuccess);

    // 用 device_id=1 recover（create 时写的是 0）→ 顺序校验失败
    cabe::SuperBlock sb2{};
    EXPECT_EQ(cabe::RecoverDeviceGroup(cfg_, 1, 1, &sb2), cabe::err::kSuperBlockDeviceIdMismatch);
}

TEST_F(SuperBlockTest, RecoverDeviceCountMismatchRejected) {
    cabe::SuperBlock sb{};
    // create 时声明属于一个 2 设备组（device_id=0, device_count=2）
    ASSERT_EQ(cabe::CreateDeviceGroup(cfg_, 0, 2, &sb), cabe::err::kSuccess);

    // 用更短的设备列表 recover（N=1）：device_id 匹配（0==0），但设备总数不符 → 干净拒开。
    cabe::SuperBlock sb2{};
    EXPECT_EQ(cabe::RecoverDeviceGroup(cfg_, 0, 1, &sb2), cabe::err::kSuperBlockDeviceCountMismatch);
}

TEST_F(SuperBlockTest, CreateIdempotent) {
    cabe::SuperBlock sb{};
    ASSERT_EQ(cabe::CreateDeviceGroup(cfg_, 0, 1, &sb), cabe::err::kSuccess);
    ASSERT_EQ(cabe::CreateDeviceGroup(cfg_, 0, 1, &sb), cabe::err::kSuccess);  // 重复 create 覆盖

    cabe::SuperBlock sb2{};
    EXPECT_EQ(cabe::RecoverDeviceGroup(cfg_, 0, 1, &sb2), cabe::err::kSuccess);
}

TEST_F(SuperBlockTest, CrcCorruptionUsesBackup) {
    cabe::SuperBlock sb{};
    ASSERT_EQ(cabe::CreateDeviceGroup(cfg_, 0, 1, &sb), cabe::err::kSuccess);

    // 篡改数据设备主份超级块（@0）一个字节 → 主份 CRC 失效
    cabe::RawDevice dev;
    ASSERT_EQ(dev.Open(data_), cabe::err::kSuccess);
    std::byte* buf = cabe::RawDevice::AllocAligned(cabe::kSuperBlockSize);
    ASSERT_NE(buf, nullptr);
    ASSERT_EQ(dev.ReadAt(0, buf, cabe::kSuperBlockSize), cabe::err::kSuccess);
    buf[100] ^= std::byte{0xFF};
    ASSERT_EQ(dev.WriteAt(0, buf, cabe::kSuperBlockSize), cabe::err::kSuccess);
    cabe::RawDevice::FreeAligned(buf);
    dev.Close();

    // recover 应用备份成功，并把主份 @0 修复回有效超级块
    cabe::SuperBlock sb2{};
    EXPECT_EQ(cabe::RecoverDeviceGroup(cfg_, 0, 1, &sb2), cabe::err::kSuccess);

    // 校验主份 @0 已被修复：重读应为有效超级块且 device_uuid 与原值一致
    cabe::RawDevice dev2;
    ASSERT_EQ(dev2.Open(data_), cabe::err::kSuccess);
    std::byte* buf2 = cabe::RawDevice::AllocAligned(cabe::kSuperBlockSize);
    ASSERT_NE(buf2, nullptr);
    ASSERT_EQ(dev2.ReadAt(0, buf2, cabe::kSuperBlockSize), cabe::err::kSuccess);
    cabe::SuperBlock primary{};
    std::memcpy(&primary, buf2, cabe::kSuperBlockSize);
    EXPECT_EQ(primary.magic, cabe::kSuperBlockMagic);
    EXPECT_EQ(std::memcmp(primary.device_uuid, sb.device_uuid, cabe::kUuidBytes), 0);
    cabe::RawDevice::FreeAligned(buf2);
    dev2.Close();
}

TEST_F(SuperBlockTest, RecoverWithoutCreateFails) {
    // 用 RawDevice 把数据设备前 8K（主备超级块）清零 → 无有效超级块
    cabe::RawDevice dev;
    ASSERT_EQ(dev.Open(data_), cabe::err::kSuccess);
    std::byte* buf = cabe::RawDevice::AllocAligned(2 * cabe::kSuperBlockSize);
    ASSERT_NE(buf, nullptr);
    std::memset(buf, 0, 2 * cabe::kSuperBlockSize);
    ASSERT_EQ(dev.WriteAt(0, buf, 2 * cabe::kSuperBlockSize), cabe::err::kSuccess);
    cabe::RawDevice::FreeAligned(buf);
    dev.Close();

    // 主备超级块均被清零（魔数=0）→ 视为未格式化/非本格式
    cabe::SuperBlock sb{};
    EXPECT_EQ(cabe::RecoverDeviceGroup(cfg_, 0, 1, &sb), cabe::err::kSuperBlockMagicMismatch);
}
