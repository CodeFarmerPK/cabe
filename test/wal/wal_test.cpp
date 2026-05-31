#include "wal/wal.h"
#include "wal/wal_frame.h"
#include "engine/engine.h"
#include "engine/options.h"
#include "common/error_code.h"
#include "common/structs.h"
#include "util/raw_device.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string GetEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : "";
}

// 从 WAL 设备读出第 block_idx 个 4K 块内第 slot 个帧（128 字节）。
cabe::WalFrame ReadWalFrame(const std::string& wal_path, std::uint64_t block_idx, std::uint32_t slot) {
    cabe::RawDevice dev;
    EXPECT_EQ(dev.Open(wal_path), cabe::err::kSuccess);
    std::byte* buf = cabe::RawDevice::AllocAligned(cabe::kWalBlockSize);
    EXPECT_NE(buf, nullptr);
    const std::uint64_t off = cabe::kDataRegionOffset + block_idx * cabe::kWalBlockSize;
    EXPECT_EQ(dev.ReadAt(off, buf, cabe::kWalBlockSize), cabe::err::kSuccess);
    cabe::WalFrame f{};
    std::memcpy(&f, buf + slot * cabe::kWalFrameSize, cabe::kWalFrameSize);
    cabe::RawDevice::FreeAligned(buf);
    dev.Close();
    return f;
}

} // namespace

// ============================================================
// 不需设备：帧编解码往返 + 校验
// ============================================================

TEST(WalFrameTest, RoundTrip) {
    const std::string key = "user:42";
    cabe::WalEntry e{};
    e.type      = cabe::WalEntryType::Put;
    e.key       = key;
    e.block     = cabe::BlockId::Make(0, 7);
    e.value_crc = 0xDEADBEEFu;
    e.timestamp = 1234567890ull;

    const cabe::WalFrame f = cabe::EncodeFrame(e, /*seq=*/5);

    // 序列化到 128 字节再读回（模拟落盘 / 读回）
    std::byte raw[cabe::kWalFrameSize];
    std::memcpy(raw, &f, cabe::kWalFrameSize);
    cabe::WalFrame g{};
    std::memcpy(&g, raw, cabe::kWalFrameSize);

    EXPECT_TRUE(cabe::VerifyFrame(g));
    EXPECT_EQ(g.magic, cabe::kWalMagic);
    EXPECT_EQ(g.entry_type, static_cast<std::uint8_t>(cabe::WalEntryType::Put));
    EXPECT_EQ(g.seq, 5u);
    EXPECT_EQ(g.block, cabe::BlockId::Make(0, 7).raw);
    EXPECT_EQ(g.value_crc, 0xDEADBEEFu);
    EXPECT_EQ(g.timestamp, 1234567890ull);
    EXPECT_EQ(g.key_len, key.size());
    EXPECT_EQ(std::memcmp(g.key, key.data(), key.size()), 0);
}

TEST(WalFrameTest, TombstoneFrame) {
    cabe::WalEntry e{};
    e.type      = cabe::WalEntryType::Delete;
    e.key       = "k";
    e.timestamp = 1;
    const cabe::WalFrame f = cabe::EncodeFrame(e, 9);
    EXPECT_TRUE(cabe::VerifyFrame(f));
    EXPECT_EQ(f.entry_type, static_cast<std::uint8_t>(cabe::WalEntryType::Delete));
    EXPECT_EQ(f.block, 0u);
    EXPECT_EQ(f.value_crc, 0u);
}

TEST(WalFrameTest, RejectsCorruption) {
    cabe::WalEntry e{};
    e.type = cabe::WalEntryType::Put;
    e.key  = "abc";
    const cabe::WalFrame f = cabe::EncodeFrame(e, 1);
    ASSERT_TRUE(cabe::VerifyFrame(f));

    cabe::WalFrame bad_magic = f;
    bad_magic.magic ^= 0xFFu;
    EXPECT_FALSE(cabe::VerifyFrame(bad_magic));

    cabe::WalFrame bad_ver = f;
    bad_ver.version = 0xFF;
    EXPECT_FALSE(cabe::VerifyFrame(bad_ver));

    cabe::WalFrame bad_body = f;            // 改一个被 CRC 覆盖的字节
    bad_body.key[0] = static_cast<std::uint8_t>(bad_body.key[0] ^ 0xFF);
    EXPECT_FALSE(cabe::VerifyFrame(bad_body));
}

// ============================================================
// 需设备：3 个 loop 设备（数据 / WAL / 快照）
// ============================================================

class WalDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_  = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            GTEST_SKIP() << "需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE";
        }
    }

    cabe::Options MakeOpts() const {
        cabe::Options opts;
        cabe::DeviceConfig cfg;
        cfg.data_path     = data_;
        cfg.wal_path      = wal_;
        cfg.snapshot_path = snap_;
        opts.devices.push_back(cfg);
        opts.create    = true;                  // create：格式化三设备
        opts.wal_level = cabe::WalLevel::Strict; // M2 显式跑级别 1
        return opts;
    }

    static std::vector<std::byte> MakeValue(std::byte fill) {
        return std::vector<std::byte>(cabe::kValueSize, fill);
    }

    std::string data_, wal_, snap_;
};

// Put 后，WAL 设备第 0 块第 0 槽是一条正确的 Put 帧。
TEST_F(WalDeviceTest, PutWritesFrame) {
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);

    const std::string key = "alpha";
    const auto value = MakeValue(std::byte{0xAB});
    ASSERT_EQ(engine.Put(key, cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    engine.Close();

    const cabe::WalFrame f = ReadWalFrame(wal_, /*block_idx=*/0, /*slot=*/0);
    ASSERT_TRUE(cabe::VerifyFrame(f));
    EXPECT_EQ(f.entry_type, static_cast<std::uint8_t>(cabe::WalEntryType::Put));
    EXPECT_EQ(f.seq, 1u);
    EXPECT_EQ(f.key_len, key.size());
    EXPECT_EQ(std::memcmp(f.key, key.data(), key.size()), 0);
    EXPECT_NE(f.value_crc, 0u);
}

// Put 再 Delete：第 0 槽 Put(seq1)、第 1 槽墓碑(seq2)。
TEST_F(WalDeviceTest, DeleteWritesTombstone) {
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);

    const std::string key = "beta";
    const auto value = MakeValue(std::byte{0xCD});
    ASSERT_EQ(engine.Put(key, cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    ASSERT_EQ(engine.Delete(key).code, cabe::err::kSuccess);
    engine.Close();

    const cabe::WalFrame put_f = ReadWalFrame(wal_, 0, 0);
    const cabe::WalFrame del_f = ReadWalFrame(wal_, 0, 1);
    ASSERT_TRUE(cabe::VerifyFrame(put_f));
    ASSERT_TRUE(cabe::VerifyFrame(del_f));
    EXPECT_EQ(put_f.entry_type, static_cast<std::uint8_t>(cabe::WalEntryType::Put));
    EXPECT_EQ(del_f.entry_type, static_cast<std::uint8_t>(cabe::WalEntryType::Delete));
    EXPECT_EQ(del_f.seq, 2u);
    EXPECT_EQ(del_f.block, 0u);
    EXPECT_EQ(del_f.value_crc, 0u);
    EXPECT_EQ(del_f.key_len, key.size());
    EXPECT_EQ(std::memcmp(del_f.key, key.data(), key.size()), 0);
}

// Engine::Put 最早一步拒绝超长 key（无副作用）。
TEST_F(WalDeviceTest, EngineRejectsLongKey) {
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);

    const std::string big(cabe::kWalKeyMax + 1, 'x');   // 85 字节
    const auto value = MakeValue(std::byte{0x01});
    EXPECT_EQ(engine.Put(big, cabe::DataView{value.data(), value.size()}).code,
              cabe::err::kWalKeyTooLong);
    engine.Close();
}

// Put 再 Get：value 经数据盘往返一致（级别 1 已 FUA 落盘）。
TEST_F(WalDeviceTest, PutThenGet) {
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);

    const std::string key = "gamma";
    const auto value = MakeValue(std::byte{0x5A});
    ASSERT_EQ(engine.Put(key, cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);

    std::vector<std::byte> out(cabe::kValueSize);
    ASSERT_EQ(engine.Get(key, cabe::DataBuffer{out.data(), out.size()}).code, cabe::err::kSuccess);
    EXPECT_EQ(out, value);
    engine.Close();
}

// 直接驱动 Wal：写满一个 4K 块后第 33 帧落到第二个块开头（seq=33）。
TEST_F(WalDeviceTest, BlockAdvance) {
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, cabe::WalLevel::Strict), cabe::err::kSuccess);

    for (int i = 0; i < 33; ++i) {
        cabe::WalEntry e{};
        e.type      = cabe::WalEntryType::Put;
        e.key       = "k";
        e.block     = cabe::BlockId::Make(0, static_cast<std::uint64_t>(i));
        e.value_crc = 1;
        e.timestamp = 1;
        ASSERT_EQ(wal.WriteWal(e), cabe::err::kSuccess);
    }
    wal.Close();

    const cabe::WalFrame f = ReadWalFrame(wal_, /*block_idx=*/1, /*slot=*/0);
    ASSERT_TRUE(cabe::VerifyFrame(f));
    EXPECT_EQ(f.seq, 33u);
    EXPECT_EQ(f.block, cabe::BlockId::Make(0, 32).raw);
}

// 直接驱动 Wal：超长 key 被 WriteWal 拒绝。
TEST_F(WalDeviceTest, WalRejectsLongKey) {
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, cabe::WalLevel::Strict), cabe::err::kSuccess);

    cabe::WalEntry e{};
    e.type = cabe::WalEntryType::Put;
    const std::string big(cabe::kWalKeyMax + 1, 'x');
    e.key = big;
    EXPECT_EQ(wal.WriteWal(e), cabe::err::kWalKeyTooLong);
    wal.Close();
}
