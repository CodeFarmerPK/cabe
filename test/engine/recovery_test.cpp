// P5M6：崩溃恢复端到端测试（设计稿 doc/P5/P5M6_recovery_design.md §13，engine 层 8 例）。
// 手法一（优雅闭环：create→写→Close→recover→验证——优雅停机是合法恢复输入）+
// 手法二（Close 后 O_DIRECT 盘面篡改，按拒开判定条件反向雕崩溃形态）。
// 需要 3 个 loop 设备：CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE。
// 既有 create 全量用例零修改全绿 = 1.4 零扰动的另一半实证（由整个测试套件承担）。

#include "engine/engine.h"
#include "engine/options.h"
#include "engine/super_block.h"
#include "snapshot/snapshot_format.h"
#include "wal/wal.h"
#include "wal/wal_frame.h"
#include "common/error_code.h"
#include "common/structs.h"
#include "util/crc32.h"
#include "util/raw_device.h"
#include "test/common/test_env.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using cabe::test::GetEnv;

class RecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_  = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            GTEST_SKIP() << "需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE";
        }
    }

    // 阈值 1M：过 WAL 容量校验（16M 设备 vs 默认 512M×2 拒开）；触发线远高于用例写入量。
    cabe::Options MakeOpts(bool create, bool verify_crc = false) const {
        cabe::Options opts;
        cabe::DeviceConfig cfg;
        cfg.data_path     = data_;
        cfg.wal_path      = wal_;
        cfg.snapshot_path = snap_;
        opts.devices.push_back(cfg);
        opts.create = create;
        opts.snapshot_threshold_bytes = 1024 * 1024;
        opts.verify_value_crc_on_recovery = verify_crc;
        return opts;
    }

    static std::vector<std::byte> MakeValue(std::byte fill) {
        return std::vector<std::byte>(cabe::kValueSize, fill);
    }

    // 断言某 key 读出的 value 全为 fill 字节。
    void ExpectValue(cabe::Engine& engine, const std::string& key, std::byte fill) const {
        std::vector<std::byte> out(cabe::kValueSize);
        ASSERT_EQ(engine.Get(key, cabe::DataBuffer{out.data(), out.size()}).code, cabe::err::kSuccess)
            << "key=" << key;
        EXPECT_EQ(out, MakeValue(fill)) << "key=" << key;
    }

    // 把两个快照槽头清零（雕"快照全失"形态——创世重放用例）。
    void WipeSnapshotHeaders() const {
        cabe::RawDevice dev;
        ASSERT_EQ(dev.Open(snap_), cabe::err::kSuccess);
        const std::uint64_t slot = cabe::SnapshotSlotSize(dev.SizeBytes());
        ASSERT_GT(slot, 0u);
        std::byte* z = cabe::RawDevice::AllocAligned(cabe::kSnapshotSlotHeaderSize);
        ASSERT_NE(z, nullptr);
        std::memset(z, 0, cabe::kSnapshotSlotHeaderSize);
        ASSERT_EQ(dev.WriteAt(cabe::kDataRegionOffset, z, cabe::kSnapshotSlotHeaderSize),
                  cabe::err::kSuccess);
        ASSERT_EQ(dev.WriteAt(cabe::kDataRegionOffset + slot, z, cabe::kSnapshotSlotHeaderSize),
                  cabe::err::kSuccess);
        cabe::RawDevice::FreeAligned(z);
        dev.Close();
    }

    // 篡改数据设备第 0 逻辑块的一个字节（雕"键在值坏"形态——级别 3 契约场景）。
    void TamperDataBlock0() const {
        cabe::RawDevice dev;
        ASSERT_EQ(dev.Open(data_), cabe::err::kSuccess);
        std::byte* buf = cabe::RawDevice::AllocAligned(4096);
        ASSERT_NE(buf, nullptr);
        ASSERT_EQ(dev.ReadAt(cabe::kDataRegionOffset, buf, 4096), cabe::err::kSuccess);
        buf[0] = buf[0] ^ std::byte{0xFF};
        ASSERT_EQ(dev.WriteAt(cabe::kDataRegionOffset, buf, 4096), cabe::err::kSuccess);
        cabe::RawDevice::FreeAligned(buf);
        dev.Close();
    }

    // 在 WAL 第 block 块第 slot 槽伪造一帧（直写盘面，雕证据矛盾）。
    void ForgeWalFrame(std::uint64_t block_idx, std::uint32_t slot, const cabe::WalFrame& f) const {
        cabe::RawDevice dev;
        ASSERT_EQ(dev.Open(wal_), cabe::err::kSuccess);
        std::byte* buf = cabe::RawDevice::AllocAligned(cabe::kWalBlockSize);
        ASSERT_NE(buf, nullptr);
        const std::uint64_t off = cabe::kDataRegionOffset + block_idx * cabe::kWalBlockSize;
        ASSERT_EQ(dev.ReadAt(off, buf, cabe::kWalBlockSize), cabe::err::kSuccess);
        std::memcpy(buf + slot * cabe::kWalFrameSize, &f, cabe::kWalFrameSize);
        ASSERT_EQ(dev.WriteAt(off, buf, cabe::kWalBlockSize), cabe::err::kSuccess);
        cabe::RawDevice::FreeAligned(buf);
        dev.Close();
    }

    std::string data_, wal_, snap_;
};

} // namespace

// 基础往返：写 + 删 → 优雅关闭 → recover → 数据与关闭前一致，恢复后引擎全功能可写（1.1 全链）。
TEST_F(RecoveryTest, RecoverBasicRoundTrip) {
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(/*create=*/true)).code, cabe::err::kSuccess);
        const auto va = MakeValue(std::byte{0xA1});
        const auto vb = MakeValue(std::byte{0xB2});
        const auto vc = MakeValue(std::byte{0xC3});
        ASSERT_EQ(engine.Put("a", cabe::DataView{va.data(), va.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("b", cabe::DataView{vb.data(), vb.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("c", cabe::DataView{vc.data(), vc.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Delete("b").code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Close().code, cabe::err::kSuccess);
    }
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts(/*create=*/false)).code, cabe::err::kSuccess);
    ExpectValue(engine, "a", std::byte{0xA1});
    ExpectValue(engine, "c", std::byte{0xC3});
    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine.Get("b", cabe::DataBuffer{out.data(), out.size()}).code,
              cabe::err::kIndexKeyNotFound) << "删除跨重启生效";

    const auto vd = MakeValue(std::byte{0xD4});                     // 恢复后全功能可写
    ASSERT_EQ(engine.Put("d", cabe::DataView{vd.data(), vd.size()}).code, cabe::err::kSuccess);
    ExpectValue(engine, "d", std::byte{0xD4});
    engine.Close();
}

// 快照基底 + WAL 增量合成：跨快照边界的覆盖/删除/新增全部正确（1.1 硬依赖②的端到端）。
TEST_F(RecoveryTest, RecoverSnapshotPlusWal) {
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(true)).code, cabe::err::kSuccess);
        const auto v1 = MakeValue(std::byte{0x11});
        const auto v2 = MakeValue(std::byte{0x22});
        ASSERT_EQ(engine.Put("k1", cabe::DataView{v1.data(), v1.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k2", cabe::DataView{v2.data(), v2.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // 基底：k1=0x11, k2=0x22
        const auto v3 = MakeValue(std::byte{0x33});
        const auto v4 = MakeValue(std::byte{0x44});
        ASSERT_EQ(engine.Put("k3", cabe::DataView{v3.data(), v3.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k1", cabe::DataView{v4.data(), v4.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Delete("k2").code, cabe::err::kSuccess);   // 增量：+k3、k1→0x44、−k2
        ASSERT_EQ(engine.Close().code, cabe::err::kSuccess);
    }
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts(false)).code, cabe::err::kSuccess);
    ExpectValue(engine, "k1", std::byte{0x44});   // WAL 增量压过快照基底

    ExpectValue(engine, "k3", std::byte{0x33});
    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine.Get("k2", cabe::DataBuffer{out.data(), out.size()}).code,
              cabe::err::kIndexKeyNotFound);
    engine.Close();
}

// 创世重放红利（3.3）：快照全失（双槽头抹掉）但环未绕 → 从 seq 1 全量重放，一字不丢。
TEST_F(RecoveryTest, RecoverAfterSlotHeadersWiped) {
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(true)).code, cabe::err::kSuccess);
        const auto v1 = MakeValue(std::byte{0x11});
        const auto v2 = MakeValue(std::byte{0x22});
        const auto v3 = MakeValue(std::byte{0x33});
        ASSERT_EQ(engine.Put("k1", cabe::DataView{v1.data(), v1.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k2", cabe::DataView{v2.data(), v2.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k3", cabe::DataView{v3.data(), v3.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Close().code, cabe::err::kSuccess);
    }
    WipeSnapshotHeaders();                                          // 快照证据全失

    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts(false)).code, cabe::err::kSuccess)
        << "环未绕 → covered=0 创世重放应成功（快照纯是加速器）";
    ExpectValue(engine, "k1", std::byte{0x11});
    ExpectValue(engine, "k2", std::byte{0x22});
    ExpectValue(engine, "k3", std::byte{0x33});
    engine.Close();
}

// 孤立块归位（6.2"空闲 = 终态补集"）：覆盖/删除释放的块在恢复后可被精确复用。
TEST_F(RecoveryTest, RecoverOrphanBlocksReusable) {
    std::uint64_t block_count = 0;
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(true)).code, cabe::err::kSuccess);
        const auto v = MakeValue(std::byte{0xEE});
        ASSERT_EQ(engine.Put("k1", cabe::DataView{v.data(), v.size()}).code, cabe::err::kSuccess);  // 块 0
        ASSERT_EQ(engine.Put("k2", cabe::DataView{v.data(), v.size()}).code, cabe::err::kSuccess);  // 块 1
        ASSERT_EQ(engine.Put("k3", cabe::DataView{v.data(), v.size()}).code, cabe::err::kSuccess);  // 块 2
        ASSERT_EQ(engine.Put("k2", cabe::DataView{v.data(), v.size()}).code, cabe::err::kSuccess);  // 块 3，释放块 1
        ASSERT_EQ(engine.Delete("k3").code, cabe::err::kSuccess);                                   // 释放块 2
        ASSERT_EQ(engine.Close().code, cabe::err::kSuccess);
    }
    {
        cabe::DeviceConfig cfg;
        cfg.data_path = data_; cfg.wal_path = wal_; cfg.snapshot_path = snap_;
        cabe::SuperBlock sb{};
        ASSERT_EQ(cabe::RecoverDeviceGroup(cfg, 0, 1, &sb), cabe::err::kSuccess);
        block_count = sb.block_count;                               // 16M 设备 = 15 块
    }

    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts(false)).code, cabe::err::kSuccess);
    // 终态活键 2 个（k1、k2）→ 空闲恰 = block_count − 2：逐一写新键直到精确耗尽。
    const auto v = MakeValue(std::byte{0xDD});
    std::uint64_t wrote = 0;
    for (std::uint64_t i = 0; i < block_count + 4; ++i) {
        const auto s = engine.Put("new-" + std::to_string(i), cabe::DataView{v.data(), v.size()});
        if (!s.ok()) {
            EXPECT_EQ(s.code, cabe::err::kEngineNoSpace);
            break;
        }
        ++wrote;
    }
    EXPECT_EQ(wrote, block_count - 2) << "孤立块（覆盖旧块 + 删除块）必须全部归位为空闲";
    ExpectValue(engine, "k1", std::byte{0xEE});
    engine.Close();
}

// 伪造墓碑：删不存在的键 = 证据矛盾 → Open 拒开（4.1）。
TEST_F(RecoveryTest, RecoverRejectsForgedTombstone) {
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(true)).code, cabe::err::kSuccess);
        const auto v = MakeValue(std::byte{0xAA});
        ASSERT_EQ(engine.Put("k1", cabe::DataView{v.data(), v.size()}).code, cabe::err::kSuccess);  // seq 1
        ASSERT_EQ(engine.Close().code, cabe::err::kSuccess);
    }
    cabe::WalEntry ghost{};
    ghost.type = cabe::WalEntryType::Delete;
    ghost.key  = "ghost";
    ghost.timestamp = 1;
    ForgeWalFrame(0, 1, cabe::EncodeFrame(ghost, /*seq=*/2));       // 紧接 seq1 的伪造墓碑

    cabe::Engine engine;
    EXPECT_EQ(engine.Open(MakeOpts(false)).code, cabe::err::kWalRecoveryCorrupted);
    EXPECT_FALSE(engine.is_open());
}

// verify_value_crc_on_recovery（4.4）：CRC 不符 ≠ 恢复失败——条目保留、Get 时如实报损；
// 开关只影响"发现时刻"（提前到恢复期记日志），不改任何行为结果。
TEST_F(RecoveryTest, RecoverVerifyValueCrcOption) {
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(true)).code, cabe::err::kSuccess);
        const auto va = MakeValue(std::byte{0xAA});
        const auto vb = MakeValue(std::byte{0xBB});
        ASSERT_EQ(engine.Put("k1", cabe::DataView{va.data(), va.size()}).code, cabe::err::kSuccess); // 块 0
        ASSERT_EQ(engine.Put("k2", cabe::DataView{vb.data(), vb.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Close().code, cabe::err::kSuccess);
    }
    TamperDataBlock0();                                             // k1 的 value 坏（键在值坏）

    for (const bool verify : {true, false}) {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(false, verify)).code, cabe::err::kSuccess)
            << "verify=" << verify << "：CRC 不符不算恢复失败（级别 3 契约）";
        std::vector<std::byte> out(cabe::kValueSize);
        EXPECT_EQ(engine.Get("k1", cabe::DataBuffer{out.data(), out.size()}).code,
                  cabe::err::kEngineDataCorrupted) << "条目保留，Get 如实报损";
        ExpectValue(engine, "k2", std::byte{0xBB});
        engine.Close();
    }
}

// 多代恢复：恢复 → 续写 → 快照 → 再恢复，跨两代的写入全部正确（1.4 终态契约 + 5.3 稠密回归）。
TEST_F(RecoveryTest, RecoverTwice) {
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(true)).code, cabe::err::kSuccess);
        const auto va = MakeValue(std::byte{0xA1});
        ASSERT_EQ(engine.Put("a", cabe::DataView{va.data(), va.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Close().code, cabe::err::kSuccess);
    }
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(false)).code, cabe::err::kSuccess);
        const auto vb = MakeValue(std::byte{0xB2});
        const auto vc = MakeValue(std::byte{0xC3});
        ASSERT_EQ(engine.Put("b", cabe::DataView{vb.data(), vb.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // 中途落一份快照 + 回收
        ASSERT_EQ(engine.Put("c", cabe::DataView{vc.data(), vc.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Close().code, cabe::err::kSuccess);
    }
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts(false)).code, cabe::err::kSuccess);
    ExpectValue(engine, "a", std::byte{0xA1});
    ExpectValue(engine, "b", std::byte{0xB2});
    ExpectValue(engine, "c", std::byte{0xC3});
    engine.Close();
}

// 快照记录语义违例（统一校验表的快照来路）：key_len=0 + 伪造 data_crc → Open 拒开（4.3/2.4）。
TEST_F(RecoveryTest, RecoverRejectsBadSnapshotRecord) {
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts(true)).code, cabe::err::kSuccess);
        const auto v = MakeValue(std::byte{0x99});
        ASSERT_EQ(engine.Put("k1", cabe::DataView{v.data(), v.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen1 → A，1 条记录
        ASSERT_EQ(engine.Close().code, cabe::err::kSuccess);
    }
    // 伪造：记录 0 的 key_len=0（合法写入方绝不产出——空 key 在写路径门口就被拒），
    // 同步重算 data_crc + 槽头 CRC，让快照裁决全部通过、由 Engine 统一校验表拦截。
    cabe::RawDevice dev;
    ASSERT_EQ(dev.Open(snap_), cabe::err::kSuccess);
    const std::uint64_t a_off = cabe::kDataRegionOffset;
    std::byte* buf = cabe::RawDevice::AllocAligned(4096);
    std::byte* hb  = cabe::RawDevice::AllocAligned(4096);
    ASSERT_NE(buf, nullptr);
    ASSERT_NE(hb, nullptr);
    ASSERT_EQ(dev.ReadAt(a_off + cabe::kSnapshotSlotHeaderSize, buf, 4096), cabe::err::kSuccess);
    cabe::SnapshotRecord rec{};
    std::memcpy(&rec, buf, sizeof rec);
    rec.key_len = 0;
    std::memcpy(buf, &rec, sizeof rec);
    ASSERT_EQ(dev.WriteAt(a_off + cabe::kSnapshotSlotHeaderSize, buf, 4096), cabe::err::kSuccess);
    cabe::SnapshotSlotHeader h{};
    ASSERT_EQ(dev.ReadAt(a_off, hb, 4096), cabe::err::kSuccess);
    std::memcpy(&h, hb, sizeof h);
    h.data_crc = ~cabe::util::CRC32CStreamUpdate(0xFFFFFFFFu,
                     cabe::DataView{buf, static_cast<std::size_t>(h.data_len)});
    h.header_crc32c = cabe::ComputeSlotHeaderCrc(h);
    std::memcpy(hb, &h, sizeof h);
    ASSERT_EQ(dev.WriteAt(a_off, hb, 4096), cabe::err::kSuccess);
    cabe::RawDevice::FreeAligned(buf);
    cabe::RawDevice::FreeAligned(hb);
    dev.Close();

    cabe::Engine engine;
    EXPECT_EQ(engine.Open(MakeOpts(false)).code, cabe::err::kSnapshotCorrupted);
    EXPECT_FALSE(engine.is_open());
}
