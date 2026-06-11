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
        // P5M5 测试基准阈值 1M：过 WAL 容量校验（16M 设备 vs 默认 512M×2 会拒开）；
        // 触发线 8192 帧远高于既有用例最大写入量（256 帧）——零行为扰动（设计 §10）。
        opts.snapshot_threshold_bytes = 1024 * 1024;
        return opts;
    }

    // P5M5 直驱 Wal 用：小阈值过容量校验（直驱无引擎、无自动快照，阈值只参与 Open 校验）。
    cabe::Options MakeWalOpts(cabe::WalLevel lvl) const {
        cabe::Options opts;
        opts.wal_level = lvl;
        opts.snapshot_threshold_bytes = 1024 * 1024;
        return opts;
    }

    // 环容量：与实现同源（WalRingSize 单一来源），测试不复抄公式。
    std::uint64_t RingBytes() const {
        cabe::RawDevice dev;
        EXPECT_EQ(dev.Open(wal_), cabe::err::kSuccess);
        const std::uint64_t r = cabe::WalRingSize(dev.SizeBytes());
        dev.Close();
        return r;
    }

    // 连写 n 帧（断言逐帧成功），seq 由 Wal 内部连续分配。
    static void PumpFrames(cabe::Wal& wal, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            cabe::WalEntry e{};
            e.type = cabe::WalEntryType::Put; e.key = "k";
            e.block = cabe::BlockId::Make(0, i & 0xFFFF); e.value_crc = 1; e.timestamp = 1;
            ASSERT_EQ(wal.WriteWal(e), cabe::err::kSuccess) << "第 " << i << " 帧";
        }
    }

    static cabe::WalEntry OneFrame() {
        cabe::WalEntry e{};
        e.type = cabe::WalEntryType::Put; e.key = "k";
        e.block = cabe::BlockId::Make(0, 1); e.value_crc = 1; e.timestamp = 1;
        return e;
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
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Strict);
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
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Strict);
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
    opts.snapshot_threshold_bytes = 1024 * 1024;   // P5M5:过 WAL 容量校验(理由见 MakeOpts)
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
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Async);   // 攒批档；wal_buffer_size 默认 32K
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

// ============================================================
// P5M5：环形队列回收（设计稿 doc/P5/P5M5_wal_ring_design.md §13）
// 测法基石：direct-Wal —— ReclaimUpTo 直调等价"快照成功"，纯 WAL 流量、秒级。
// ============================================================

// 环几何公式（不需设备）：常规 / 取整 / 过小设备 → 0。
TEST(WalRingGeometryTest, RingSizeFormula) {
    EXPECT_EQ(cabe::WalRingSize(0), 0u);
    EXPECT_EQ(cabe::WalRingSize(cabe::kDataRegionOffset), 0u);                 // 只装得下超级块
    EXPECT_EQ(cabe::WalRingSize(cabe::kDataRegionOffset + 4096), 4096u);
    EXPECT_EQ(cabe::WalRingSize(cabe::kDataRegionOffset + 4096 + 100), 4096u); // 零头向下取整
    EXPECT_EQ(cabe::WalRingSize(16ull << 20), (16ull << 20) - cabe::kDataRegionOffset);
}

// 容量校验：阈值过大（默认 512M×2 ≫ 16M 设备）→ Open 拒开 kDeviceTooSmall。
TEST_F(WalDeviceTest, OpenRejectsSmallRing) {
    cabe::Options opts;                       // 默认阈值 512M → 需求 1G ≫ 16M 环
    opts.wal_level = cabe::WalLevel::Strict;
    cabe::Wal wal;
    EXPECT_EQ(wal.Open(wal_, &opts), cabe::err::kDeviceTooSmall);
}

// 变体 Y 无空洞：提前刷出"整块推进、半块留窗"，续写后补零槽被新帧填上（设计 §6.1）。
TEST_F(WalDeviceTest, EarlyFlushNoHoles) {
    ZeroWalRegion(wal_, 3);
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Async);
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    PumpFrames(wal, 5);                                        // seq 1..5
    ASSERT_EQ(wal.Flush(), cabe::err::kSuccess);               // 提前刷出（不足一块）
    EXPECT_EQ(wal.reclaim_boundary(), cabe::kDataRegionOffset) // 推进 0：半块留窗
        << "变体 Y：不足整块的提前刷出不推进";
    for (std::uint32_t i = 0; i < 5; ++i) {
        const cabe::WalFrame f = ReadWalFrame(wal_, 0, i);
        ASSERT_TRUE(cabe::VerifyFrame(f));
        EXPECT_EQ(f.seq, i + 1u);
    }
    EXPECT_FALSE(cabe::VerifyFrame(ReadWalFrame(wal_, 0, 5))); // 此刻槽 5 是补零

    PumpFrames(wal, 28);                                       // seq 6..33（块 0 满 + 块 1 一帧）
    ASSERT_EQ(wal.Flush(), cabe::err::kSuccess);
    EXPECT_EQ(wal.reclaim_boundary(), cabe::kDataRegionOffset + cabe::kWalBlockSize)
        << "推进 1 整块，留窗 1 帧";
    for (std::uint32_t i = 0; i < cabe::kWalFramesPerBlock; ++i) {
        const cabe::WalFrame f = ReadWalFrame(wal_, 0, i);     // 块 0 整块紧凑——无空洞
        ASSERT_TRUE(cabe::VerifyFrame(f)) << "槽 " << i;
        EXPECT_EQ(f.seq, i + 1u);
    }
    const cabe::WalFrame f33 = ReadWalFrame(wal_, 1, 0);
    ASSERT_TRUE(cabe::VerifyFrame(f33));
    EXPECT_EQ(f33.seq, 33u);
    wal.Close();
}

// 回收：head 跳跃前移；空回收幂等；全量回收（boundary == tail）取等放行（设计 §7.1/§7.2）。
TEST_F(WalDeviceTest, ReclaimAdvancesHead) {
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Async);
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);
    EXPECT_EQ(wal.head_off(), cabe::kDataRegionOffset);        // 空环：head == tail

    PumpFrames(wal, 256);                                      // 整窗（32K）自动刷出
    const std::uint64_t b = wal.reclaim_boundary();
    EXPECT_EQ(b, cabe::kDataRegionOffset + opts.wal_buffer_size);

    ASSERT_EQ(wal.ReclaimUpTo(b), cabe::err::kSuccess);        // 全量回收
    EXPECT_EQ(wal.head_off(), b);
    ASSERT_EQ(wal.ReclaimUpTo(b), cabe::err::kSuccess);        // 空回收幂等
    EXPECT_EQ(wal.head_off(), b);
    wal.Close();
}

// 回收校验：倒退 / 越 tail / 不对齐 / 环外 → kWalInvalidReclaim 且 head 纹丝不动（设计 §7.2）。
TEST_F(WalDeviceTest, ReclaimRejectsInvalid) {
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Async);
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);
    PumpFrames(wal, 256);                                      // tail = start + 32K
    const std::uint64_t b = wal.reclaim_boundary();
    ASSERT_EQ(wal.ReclaimUpTo(b), cabe::err::kSuccess);        // head = start + 32K
    PumpFrames(wal, 16);                                       // 攒着不刷：tail 不动

    const std::uint64_t ring_end = cabe::kDataRegionOffset + RingBytes();
    EXPECT_EQ(wal.ReclaimUpTo(b + 1), cabe::err::kWalInvalidReclaim);        // ① 不对齐
    EXPECT_EQ(wal.ReclaimUpTo(0), cabe::err::kWalInvalidReclaim);            // ② 环外（低）
    EXPECT_EQ(wal.ReclaimUpTo(ring_end), cabe::err::kWalInvalidReclaim);     // ② 环外（高）
    EXPECT_EQ(wal.ReclaimUpTo(cabe::kDataRegionOffset),                      // ③ 倒退
              cabe::err::kWalInvalidReclaim);
    EXPECT_EQ(wal.ReclaimUpTo(b + cabe::kWalBlockSize),                      // ③ 越过 tail
              cabe::err::kWalInvalidReclaim);
    EXPECT_EQ(wal.head_off(), b);                              // 保守失败：head 不动
    wal.Close();
}

// 绕圈：写 → 模拟快照回收 → 续写越过环尾绕回；seq 不重置；环上残留旧帧可与新帧分辨
//（设计 §4/§6.3；同时隐含验证贴缝窗口截短——精确落位证明无任何跨缝写出）。
TEST_F(WalDeviceTest, RingWrapAround) {
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Async);
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    const std::uint64_t ring = RingBytes();
    const std::uint64_t total_frames = ring / cabe::kWalFrameSize;     // 满环帧数
    const std::size_t   kPerRound = 2048;                              // 64 块/轮（整块数）
    const std::size_t   rounds = static_cast<std::size_t>(total_frames / kPerRound) + 1;
    for (std::size_t r = 0; r < rounds; ++r) {
        PumpFrames(wal, kPerRound);
        ASSERT_EQ(wal.Flush(), cabe::err::kSuccess);
        ASSERT_EQ(wal.ReclaimUpTo(wal.reclaim_boundary()), cabe::err::kSuccess); // 模拟快照成功
    }
    const std::uint64_t written = static_cast<std::uint64_t>(rounds) * kPerRound * cabe::kWalFrameSize;
    ASSERT_GT(written, ring) << "测试前提：写入量须越过环尾";
    const std::uint64_t expect_tail = cabe::kDataRegionOffset + written % ring;
    EXPECT_EQ(wal.reclaim_boundary(), expect_tail);            // 紧凑性：tail = 总字节 mod 环
    EXPECT_EQ(wal.head_off(), expect_tail);                    // 每轮全量回收 → head 跟上

    // 环头块 0 = 第二圈新帧；变体 Y 紧凑 ⇒ 第 k 帧在 (k-1)*128 mod ring ⇒ 槽 0 帧 seq = 满环帧数+1
    const cabe::WalFrame f0 = ReadWalFrame(wal_, 0, 0);
    ASSERT_TRUE(cabe::VerifyFrame(f0));
    EXPECT_EQ(f0.seq, total_frames + 1) << "seq 绕圈不重置 + 帧流紧凑";

    // tail 之后两块：第一圈残留（合法旧帧，seq 小）——3.3 三分类中的"残留"，靠 seq 分辨
    const std::uint64_t stale_block = (expect_tail - cabe::kDataRegionOffset) / cabe::kWalBlockSize + 2;
    const cabe::WalFrame fs = ReadWalFrame(wal_, stale_block, 0);
    ASSERT_TRUE(cabe::VerifyFrame(fs));
    EXPECT_EQ(fs.seq, stale_block * cabe::kWalFramesPerBlock + 1);
    EXPECT_LT(fs.seq, f0.seq);
    wal.Close();
}

// 贴缝窗口截短：距环尾 4 块时窗口收口为 16K，攒满恰好顶到环尾、绕回，余帧落环头（设计 §4）。
TEST_F(WalDeviceTest, WindowShrinkAtSeam) {
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Async);
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    const std::uint64_t ring = RingBytes();
    const std::uint64_t total_blocks = ring / cabe::kWalBlockSize;
    const std::uint64_t pre_frames = (total_blocks - 4) * cabe::kWalFramesPerBlock;

    // 推进到距环尾 4 块处（沿途轮次回收，确保不满；每轮 2048 帧 = 64 整块）
    std::uint64_t remain = pre_frames;
    while (remain > 0) {
        const std::size_t batch = static_cast<std::size_t>(std::min<std::uint64_t>(remain, 2048));
        PumpFrames(wal, batch);
        ASSERT_EQ(wal.Flush(), cabe::err::kSuccess);
        ASSERT_EQ(wal.ReclaimUpTo(wal.reclaim_boundary()), cabe::err::kSuccess);
        remain -= batch;
    }
    const std::uint64_t seam_pos = cabe::kDataRegionOffset + ring - 4 * cabe::kWalBlockSize;
    ASSERT_EQ(wal.reclaim_boundary(), seam_pos);

    // 窗口截短为 16K = 128 帧：写 130 帧 → 第 128 帧攒满截短窗自动刷出、恰到环尾、绕回；
    // 余 2 帧进环头新窗（缓冲内），显式 Flush 写出（留窗、推进 0）。
    PumpFrames(wal, 130);
    ASSERT_EQ(wal.Flush(), cabe::err::kSuccess);
    EXPECT_EQ(wal.reclaim_boundary(), cabe::kDataRegionOffset) << "绕回后停在环头块（留窗 2 帧）";

    const cabe::WalFrame f_last = ReadWalFrame(wal_, total_blocks - 1, cabe::kWalFramesPerBlock - 1);
    ASSERT_TRUE(cabe::VerifyFrame(f_last));
    EXPECT_EQ(f_last.seq, pre_frames + 128) << "环尾最后一槽被截短窗恰好填满，无跨缝";
    const cabe::WalFrame g0 = ReadWalFrame(wal_, 0, 0);
    const cabe::WalFrame g1 = ReadWalFrame(wal_, 0, 1);
    ASSERT_TRUE(cabe::VerifyFrame(g0));
    ASSERT_TRUE(cabe::VerifyFrame(g1));
    EXPECT_EQ(g0.seq, pre_frames + 129);
    EXPECT_EQ(g1.seq, pre_frames + 130);
    EXPECT_FALSE(cabe::VerifyFrame(ReadWalFrame(wal_, 0, 2)));  // 留窗补零
    wal.Close();
}

// 攒批档写满：不回收一路写到 kWalFull——恒留一块边界精确（容量 = 环 − 4K）；
// 回收后惰性重开窗、写入复活（救援数学的 Wal 级验证；设计 §8.1/§8.2/D12）。
TEST_F(WalDeviceTest, WalFullBatchModeAndReclaimRevives) {
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Async);
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    const std::uint64_t ring = RingBytes();
    const std::uint64_t cap_frames = (ring - cabe::kWalBlockSize) / cabe::kWalFrameSize;  // 恒留一块
    PumpFrames(wal, static_cast<std::size_t>(cap_frames));     // 恰好容量内全部成功

    EXPECT_EQ(wal.WriteWal(OneFrame()), cabe::err::kWalFull);  // 超出 → 满
    EXPECT_EQ(wal.last_seq(), cap_frames) << "被拒帧 seq 未消耗";
    EXPECT_EQ(wal.reclaim_boundary(), cabe::kDataRegionOffset + ring - cabe::kWalBlockSize)
        << "tail 停在保留块上（块本身未写）";
    EXPECT_EQ(wal.head_off(), cabe::kDataRegionOffset);

    // 模拟快照成功 → 全量回收 → 下一次写惰性重开窗、复活
    ASSERT_EQ(wal.Flush(), cabe::err::kSuccess);               // 满态下窗口已清，空操作
    ASSERT_EQ(wal.ReclaimUpTo(wal.reclaim_boundary()), cabe::err::kSuccess);
    EXPECT_EQ(wal.WriteWal(OneFrame()), cabe::err::kSuccess);
    EXPECT_EQ(wal.last_seq(), cap_frames + 1);
    wal.Close();
}

// 同步档写满：拒绝点在"块满推进前"——帧未编码、seq 未耗；容量边界与攒批档同一条（恒留一块）。
TEST_F(WalDeviceTest, WalFullSyncMode) {
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Async);   // 先攒批快速填充
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    const std::uint64_t ring = RingBytes();
    const std::uint64_t blocks = ring / cabe::kWalBlockSize;
    const std::uint64_t batch_frames = (blocks - 2) * cabe::kWalFramesPerBlock;  // 留最后一个可用块
    PumpFrames(wal, static_cast<std::size_t>(batch_frames));
    ASSERT_EQ(wal.Flush(), cabe::err::kSuccess);
    ASSERT_EQ(wal.reclaim_boundary(), cabe::kDataRegionOffset + (blocks - 2) * cabe::kWalBlockSize);

    // 切同步档（直驱等价于 Engine 收紧契约：先刷净再改级别；Wal 现读 opts_）
    opts.wal_level = cabe::WalLevel::Strict;
    PumpFrames(wal, cabe::kWalFramesPerBlock);                 // 最后一个可用块写满 32 帧
    EXPECT_EQ(wal.WriteWal(OneFrame()), cabe::err::kWalFull);  // 第 33 帧：推进进保留块被拒
    EXPECT_EQ(wal.last_seq(), batch_frames + cabe::kWalFramesPerBlock)
        << "= (块数−1)×32 = 恒留一块下的精确容量；被拒帧 seq 未耗";
    wal.Close();
}
