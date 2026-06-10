#include "wal/wal.h"
#include "wal/wal_frame.h"
#include "engine/engine.h"
#include "engine/options.h"
#include "common/error_code.h"
#include "common/structs.h"
#include "util/raw_device.h"
#include "test/common/test_env.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using cabe::test::GetEnv;   // P5M4：收敛到共享测试头（原各文件逐字拷贝）

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

// 把 WAL 日志区前 n_blocks 个 4K 块清零（避免 loop 设备残留帧让"攒批未刷"判定误报）。
void ZeroWalRegion(const std::string& wal_path, std::size_t n_blocks) {
    cabe::RawDevice dev;
    ASSERT_EQ(dev.Open(wal_path), cabe::err::kSuccess);
    std::byte* buf = cabe::RawDevice::AllocAligned(cabe::kWalBlockSize);
    ASSERT_NE(buf, nullptr);
    std::memset(buf, 0, cabe::kWalBlockSize);
    for (std::size_t i = 0; i < n_blocks; ++i) {
        ASSERT_EQ(dev.WriteAt(cabe::kDataRegionOffset + i * cabe::kWalBlockSize, buf, cabe::kWalBlockSize),
                  cabe::err::kSuccess);
    }
    cabe::RawDevice::FreeAligned(buf);
    dev.Close();
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

    cabe::Options MakeOpts(cabe::WalLevel lvl = cabe::WalLevel::Strict) const {
        cabe::Options opts;
        cabe::DeviceConfig cfg;
        cfg.data_path     = data_;
        cfg.wal_path      = wal_;
        cfg.snapshot_path = snap_;
        opts.devices.push_back(cfg);
        opts.create    = true;                  // create：格式化三设备
        opts.wal_level = lvl;
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
    cabe::Options opts;
    opts.wal_level = cabe::WalLevel::Strict;
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

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
    cabe::Options opts;
    opts.wal_level = cabe::WalLevel::Strict;
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    cabe::WalEntry e{};
    e.type = cabe::WalEntryType::Put;
    const std::string big(cabe::kWalKeyMax + 1, 'x');
    e.key = big;
    EXPECT_EQ(wal.WriteWal(e), cabe::err::kWalKeyTooLong);
    wal.Close();
}

// ============================================================
// P5M3：级别 2/3/4 的可观测时机
// ============================================================

// 攒批档（级别 4）：Put 后帧还在缓冲、盘上为空；Close 触发 Flush 后才落盘。
TEST_F(WalDeviceTest, Level4Buffers) {
    ZeroWalRegion(wal_, 2);   // 清零日志区前两块，避免 loop 设备残留误判
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts(cabe::WalLevel::Async)).code, cabe::err::kSuccess);

    const std::string key = "lvl4";
    const auto value = MakeValue(std::byte{0x44});
    ASSERT_EQ(engine.Put(key, cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);

    EXPECT_FALSE(cabe::VerifyFrame(ReadWalFrame(wal_, 0, 0))) << "攒批档 Put 后帧不应立刻落盘";

    engine.Close();   // Close → Flush → 帧落盘
    const cabe::WalFrame f = ReadWalFrame(wal_, 0, 0);
    ASSERT_TRUE(cabe::VerifyFrame(f));
    EXPECT_EQ(f.entry_type, static_cast<std::uint8_t>(cabe::WalEntryType::Put));
    EXPECT_EQ(f.key_len, key.size());
}

// 默认级别 = WalSync（3）：WAL 同步，Put 后帧立刻在盘。
TEST_F(WalDeviceTest, DefaultLevelWalSync) {
    ZeroWalRegion(wal_, 2);
    cabe::Options opts;
    cabe::DeviceConfig cfg;
    cfg.data_path = data_; cfg.wal_path = wal_; cfg.snapshot_path = snap_;
    opts.devices.push_back(cfg);
    opts.create = true;   // 不设 wal_level → 默认 WalSync=3
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(opts).code, cabe::err::kSuccess);

    const std::string key = "deflt";
    const auto value = MakeValue(std::byte{0x33});
    ASSERT_EQ(engine.Put(key, cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);

    const cabe::WalFrame f = ReadWalFrame(wal_, 0, 0);
    ASSERT_TRUE(cabe::VerifyFrame(f)) << "级别 3 应 WAL 同步，帧立刻在盘";
    EXPECT_EQ(f.entry_type, static_cast<std::uint8_t>(cabe::WalEntryType::Put));
    engine.Close();
}

// 切档收紧（级别 4 → 1）：SetWalLevel 先刷攒批缓冲，帧落盘。
TEST_F(WalDeviceTest, FlushOnTighten) {
    ZeroWalRegion(wal_, 2);
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts(cabe::WalLevel::Async)).code, cabe::err::kSuccess);

    const std::string key = "tighten";
    const auto value = MakeValue(std::byte{0x55});
    ASSERT_EQ(engine.Put(key, cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    EXPECT_FALSE(cabe::VerifyFrame(ReadWalFrame(wal_, 0, 0)));   // 攒批：还没落盘

    ASSERT_EQ(engine.SetWalLevel(cabe::WalLevel::Strict).code, cabe::err::kSuccess);

    const cabe::WalFrame f = ReadWalFrame(wal_, 0, 0);
    ASSERT_TRUE(cabe::VerifyFrame(f)) << "收紧档应刷净缓冲";
    EXPECT_EQ(f.key_len, key.size());
    engine.Close();
}

// 读己之写：四个级别下 Put 完紧接 Get 都读回原值（进程内，与级别无关）。
TEST_F(WalDeviceTest, ReadYourWritesAllLevels) {
    const cabe::WalLevel levels[] = {
        cabe::WalLevel::Strict, cabe::WalLevel::ValueSync,
        cabe::WalLevel::WalSync, cabe::WalLevel::Async,
    };
    for (cabe::WalLevel lvl : levels) {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(lvl)).code, cabe::err::kSuccess);
        const std::string key = "ryw";
        const auto value = MakeValue(std::byte{0x77});
        ASSERT_EQ(engine.Put(key, cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        std::vector<std::byte> out(cabe::kValueSize);
        ASSERT_EQ(engine.Get(key, cabe::DataBuffer{out.data(), out.size()}).code, cabe::err::kSuccess);
        EXPECT_EQ(out, value) << "level=" << static_cast<int>(lvl);
        engine.Close();
    }
}

// 攒批档"缓冲攒满自动刷出"：直接驱动 Wal，连写满一整块缓冲（默认 32K=256 帧），
// 验证满前不在盘、第 256 帧触发 Flush 后整批落盘（覆盖 wal.cpp 满判定 + 多块刷出算术）。
TEST_F(WalDeviceTest, FlushOnBufferFull) {
    cabe::Options opts;
    opts.wal_level = cabe::WalLevel::Async;   // 攒批档；wal_buffer_size 默认 32K
    const std::size_t cap = opts.wal_buffer_size / cabe::kWalFrameSize;    // 256 帧
    const std::size_t blocks = opts.wal_buffer_size / cabe::kWalBlockSize; // 8 块
    ZeroWalRegion(wal_, blocks + 1);          // 清零将被刷的整块缓冲区域

    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    // 写前 cap-1 帧：缓冲未满，盘上为空
    for (std::size_t i = 0; i + 1 < cap; ++i) {
        cabe::WalEntry e{};
        e.type = cabe::WalEntryType::Put; e.key = "k";
        e.block = cabe::BlockId::Make(0, i); e.value_crc = 1; e.timestamp = 1;
        ASSERT_EQ(wal.WriteWal(e), cabe::err::kSuccess);
    }
    EXPECT_FALSE(cabe::VerifyFrame(ReadWalFrame(wal_, 0, 0))) << "攒批未满，不应落盘";

    // 写第 cap 帧：触发自动刷出，整批落盘
    cabe::WalEntry last_e{};
    last_e.type = cabe::WalEntryType::Put; last_e.key = "k";
    last_e.block = cabe::BlockId::Make(0, cap - 1); last_e.value_crc = 1; last_e.timestamp = 1;
    ASSERT_EQ(wal.WriteWal(last_e), cabe::err::kSuccess);
    wal.Close();

    const cabe::WalFrame first = ReadWalFrame(wal_, 0, 0);                  // block0 slot0
    ASSERT_TRUE(cabe::VerifyFrame(first));
    EXPECT_EQ(first.seq, 1u);
    const std::uint32_t fpb = static_cast<std::uint32_t>(cabe::kWalFramesPerBlock);
    const cabe::WalFrame last = ReadWalFrame(wal_, blocks - 1, fpb - 1);    // 最后一块最后一槽
    ASSERT_TRUE(cabe::VerifyFrame(last));
    EXPECT_EQ(last.seq, static_cast<std::uint64_t>(cap));                   // seq=256
}
