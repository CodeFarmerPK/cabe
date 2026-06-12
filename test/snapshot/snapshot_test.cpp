#include "snapshot/snapshot.h"
#include "snapshot/snapshot_format.h"
#include "engine/engine.h"
#include "engine/options.h"
#include "common/error_code.h"
#include "common/structs.h"
#include "util/raw_device.h"
#include "util/crc32.h"
#include "util/util.h"
#include "test/common/test_env.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

using cabe::test::GetEnv;   // P5M4：收敛到共享测试头（原各文件逐字拷贝）

} // namespace

// ============================================================
// 不需设备：格式编解码 + 校验 + 流式 CRC 一致性
// ============================================================

TEST(SnapshotFormatTest, RecordRoundTrip) {
    const std::string key = "user:42";
    cabe::ValueMeta meta{};
    meta.block     = cabe::BlockId::Make(0, 7);
    meta.timestamp = 1234567890ull;
    meta.crc       = 0xDEADBEEFu;
    meta.state     = cabe::ValueState::Active;

    const cabe::SnapshotRecord r = cabe::EncodeSnapshotRecord(key, meta);

    // 序列化到 128 字节再读回（模拟落盘 / 读回）
    std::byte raw[cabe::kSnapshotRecordSize];
    std::memcpy(raw, &r, cabe::kSnapshotRecordSize);
    cabe::SnapshotRecord g{};
    std::memcpy(&g, raw, cabe::kSnapshotRecordSize);

    EXPECT_EQ(g.key_len, key.size());
    EXPECT_EQ(std::memcmp(g.key, key.data(), key.size()), 0);
    EXPECT_EQ(g.block, cabe::BlockId::Make(0, 7).raw);
    EXPECT_EQ(g.timestamp, 1234567890ull);
    EXPECT_EQ(g.value_crc, 0xDEADBEEFu);
    EXPECT_EQ(g.state, static_cast<std::uint8_t>(cabe::ValueState::Active));
    // 短 key：尾部补零
    EXPECT_EQ(g.key[key.size()], 0u);
}

TEST(SnapshotFormatTest, SlotHeaderVerify) {
    cabe::SnapshotSlotHeader h{};
    h.magic       = cabe::kSnapshotSlotMagic;
    h.version     = cabe::kSnapshotSlotVersion;
    h.generation  = 5;
    h.covered_seq = 100;
    h.entry_count = 3;
    h.data_len    = 3 * cabe::kSnapshotRecordSize;
    h.data_crc    = 0x12345678u;
    h.header_crc32c = cabe::ComputeSlotHeaderCrc(h);

    EXPECT_TRUE(cabe::VerifySlotHeader(h));

    cabe::SnapshotSlotHeader bad_magic = h;
    bad_magic.magic ^= 0xFFu;
    EXPECT_FALSE(cabe::VerifySlotHeader(bad_magic));

    cabe::SnapshotSlotHeader bad_ver = h;
    bad_ver.version = 0xFF;
    EXPECT_FALSE(cabe::VerifySlotHeader(bad_ver));

    cabe::SnapshotSlotHeader bad_body = h;     // 改一个被 CRC 覆盖的字段
    bad_body.covered_seq ^= 0xFFu;
    EXPECT_FALSE(cabe::VerifySlotHeader(bad_body));
}

// 流式增量 CRC（分块）必须与一次性 CRC32（整段）一致。
TEST(SnapshotFormatTest, StreamCrcMatchesOneShot) {
    std::vector<std::byte> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = std::byte(i * 7 + 1);

    const std::uint32_t one_shot = cabe::util::CRC32(cabe::DataView{data.data(), data.size()});

    // 分三块累积
    std::uint32_t st = 0xFFFFFFFFu;
    st = cabe::util::CRC32CStreamUpdate(st, cabe::DataView{data.data(), 300});
    st = cabe::util::CRC32CStreamUpdate(st, cabe::DataView{data.data() + 300, 400});
    st = cabe::util::CRC32CStreamUpdate(st, cabe::DataView{data.data() + 700, 300});
    EXPECT_EQ(~st, one_shot);
}

// ============================================================
// 需设备：3 个 loop 设备（数据 / WAL / 快照）
// ============================================================

class SnapshotDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_  = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            GTEST_SKIP() << "需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE";
        }
        ZeroSnapshotDataRegion();   // 清掉 loop 设备上轮残留（设计 §13），防跨用例数据区误判
    }

    // 把快照设备 8K（超级块）之后整个清零：1 MiB 块顺序写，32M 测试设备瞬时完成。
    // create 只清两个槽头，数据区残留要靠这里——否则"读另一槽数据区"类用例会拿上轮字节误判。
    void ZeroSnapshotDataRegion() const {
        cabe::RawDevice dev;
        ASSERT_EQ(dev.Open(snap_), cabe::err::kSuccess);
        const std::uint64_t size = dev.SizeBytes();
        const std::size_t chunk = 1024 * 1024;
        std::byte* z = cabe::RawDevice::AllocAligned(chunk);
        ASSERT_NE(z, nullptr);
        std::memset(z, 0, chunk);
        for (std::uint64_t off = cabe::kDataRegionOffset; off < size;) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - off));
            ASSERT_EQ(dev.WriteAt(off, z, n), cabe::err::kSuccess);
            off += n;
        }
        cabe::RawDevice::FreeAligned(z);
        dev.Close();
    }

    // P5M5：默认阈值从 512M 改为 1M——过 WAL 容量校验（16M 设备 vs 512M×2 会拒开）；
    // 触发线 8192 帧远高于本文件常规用例写入量,故意触发的用例自带更小阈值。
    cabe::Options MakeOpts(std::uint64_t threshold = 1024 * 1024) const {
        cabe::Options opts;
        cabe::DeviceConfig cfg;
        cfg.data_path     = data_;
        cfg.wal_path      = wal_;
        cfg.snapshot_path = snap_;
        opts.devices.push_back(cfg);
        opts.create = true;
        opts.snapshot_threshold_bytes = threshold;
        return opts;
    }

    static std::vector<std::byte> MakeValue(std::byte fill) {
        return std::vector<std::byte>(cabe::kValueSize, fill);
    }

    // A/B 槽起点：直接用与 Snapshot::Open 共享的 SnapshotSlotSize（公式单一来源，不再复抄）。
    void Layout(std::uint64_t* a_off, std::uint64_t* b_off) const {
        cabe::RawDevice dev;
        ASSERT_EQ(dev.Open(snap_), cabe::err::kSuccess);
        const std::uint64_t slot = cabe::SnapshotSlotSize(dev.SizeBytes());
        dev.Close();
        ASSERT_GT(slot, 0u);
        *a_off = cabe::kDataRegionOffset;
        *b_off = cabe::kDataRegionOffset + slot;
    }

    cabe::SnapshotSlotHeader ReadHeader(std::uint64_t slot_off) const {
        cabe::RawDevice dev;
        EXPECT_EQ(dev.Open(snap_), cabe::err::kSuccess);
        std::byte* buf = cabe::RawDevice::AllocAligned(cabe::kSnapshotSlotHeaderSize);
        EXPECT_NE(buf, nullptr);
        EXPECT_EQ(dev.ReadAt(slot_off, buf, cabe::kSnapshotSlotHeaderSize), cabe::err::kSuccess);
        cabe::SnapshotSlotHeader h{};
        std::memcpy(&h, buf, sizeof h);
        cabe::RawDevice::FreeAligned(buf);
        dev.Close();
        return h;
    }

    // 读出该槽的数据区，按 data_len 重算 data_crc 并与槽头比对。
    bool DataCrcMatches(std::uint64_t slot_off, const cabe::SnapshotSlotHeader& h) const {
        const std::size_t data_bytes = static_cast<std::size_t>(h.entry_count) * cabe::kSnapshotRecordSize;
        if (data_bytes == 0) {
            return h.data_crc == (~cabe::util::CRC32CStreamUpdate(0xFFFFFFFFu, cabe::DataView{}));
        }
        const std::size_t aligned = cabe::util::AlignUp(data_bytes, cabe::kSnapshotBlockSize);
        cabe::RawDevice dev;
        EXPECT_EQ(dev.Open(snap_), cabe::err::kSuccess);
        std::byte* buf = cabe::RawDevice::AllocAligned(aligned);
        EXPECT_NE(buf, nullptr);
        EXPECT_EQ(dev.ReadAt(slot_off + cabe::kSnapshotSlotHeaderSize, buf, aligned), cabe::err::kSuccess);
        const std::uint32_t crc =
            ~cabe::util::CRC32CStreamUpdate(0xFFFFFFFFu, cabe::DataView{buf, data_bytes});
        cabe::RawDevice::FreeAligned(buf);
        dev.Close();
        return crc == h.data_crc;
    }

    std::string data_, wal_, snap_;
};

// 手动 Snapshot：写出一份到 A 槽，槽头字段正确、data_crc 自洽、covered_seq = WAL 末序号。
TEST_F(SnapshotDeviceTest, WriteSnapshotEndToEnd) {
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);

    const auto value = MakeValue(std::byte{0xAB});
    ASSERT_EQ(engine.Put("alpha", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    ASSERT_EQ(engine.Put("beta",  cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    ASSERT_EQ(engine.Put("gamma", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);

    ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);   // 手动触发
    engine.Close();

    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);

    const cabe::SnapshotSlotHeader h = ReadHeader(a_off);   // 首份写 A 槽
    EXPECT_TRUE(cabe::VerifySlotHeader(h));
    EXPECT_EQ(h.generation, 1u);
    EXPECT_EQ(h.entry_count, 3u);
    EXPECT_EQ(h.covered_seq, 3u);                           // 3 个 Put → WAL seq 1..3
    EXPECT_EQ(h.data_len, 3u * cabe::kSnapshotRecordSize);
    EXPECT_TRUE(DataCrcMatches(a_off, h));
}

// 连做两份快照：A(gen1) → B(gen2)，交替写非活跃槽、代际递增。
TEST_F(SnapshotDeviceTest, AlternateSlotsAndGeneration) {
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);

    const auto value = MakeValue(std::byte{0xCD});
    ASSERT_EQ(engine.Put("k1", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);   // → A，gen 1
    ASSERT_EQ(engine.Put("k2", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);   // → B，gen 2
    engine.Close();

    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);

    const cabe::SnapshotSlotHeader ha = ReadHeader(a_off);
    const cabe::SnapshotSlotHeader hb = ReadHeader(b_off);
    ASSERT_TRUE(cabe::VerifySlotHeader(ha));
    ASSERT_TRUE(cabe::VerifySlotHeader(hb));
    EXPECT_EQ(ha.generation, 1u);
    EXPECT_EQ(hb.generation, 2u);
    EXPECT_GT(hb.covered_seq, ha.covered_seq);   // B 覆盖到更晚的写
    EXPECT_TRUE(DataCrcMatches(a_off, ha));
    EXPECT_TRUE(DataCrcMatches(b_off, hb));
}

// 大小阈值自动触发：阈值设小，写够量后无需手动调用就有快照落到 A 槽。
TEST_F(SnapshotDeviceTest, ThresholdAutoTriggers) {
    cabe::Engine engine;
    // 阈值 256 字节 = 2 帧：第 2 次 Put 后 (2-0)*128=256 ≥ 256 → 自动触发。
    ASSERT_EQ(engine.Open(MakeOpts(/*threshold=*/256)).code, cabe::err::kSuccess);

    const auto value = MakeValue(std::byte{0xEE});
    ASSERT_EQ(engine.Put("a", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    ASSERT_EQ(engine.Put("b", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    engine.Close();

    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    const cabe::SnapshotSlotHeader h = ReadHeader(a_off);   // 自动触发写 A 槽
    EXPECT_TRUE(cabe::VerifySlotHeader(h));
    EXPECT_EQ(h.generation, 1u);
    EXPECT_EQ(h.entry_count, 2u);                           // 触发时索引有 2 个活键
    EXPECT_TRUE(DataCrcMatches(a_off, h));
}

// 阈值负半边：不够量 → 不触发——两槽头都仍是 create 清空后的无效态。
TEST_F(SnapshotDeviceTest, ThresholdNotReachedNoSnapshot) {
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);   // 默认阈值 512M，2 帧 = 256 字节远不够

    const auto value = MakeValue(std::byte{0x5A});
    ASSERT_EQ(engine.Put("x", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    ASSERT_EQ(engine.Put("y", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
    engine.Close();

    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    EXPECT_FALSE(cabe::VerifySlotHeader(ReadHeader(a_off)));
    EXPECT_FALSE(cabe::VerifySlotHeader(ReadHeader(b_off)));
}

// 端到端内容核对：把盘上记录解码回来，与 Put 进去的内容逐字段比对。
// （DataCrcMatches 是"读回字节重算 CRC"的循环验证，查不出字段错位/漏赋——此用例补内容级核对：
//   字段若错位（如 block/timestamp 互换），timestamp 会变成小整数、必然跌出时间窗。）
TEST_F(SnapshotDeviceTest, RecordsDecodeMatchPuts) {
    struct Expect { std::string key; std::uint32_t value_crc; };
    std::vector<Expect> expects;

    const std::uint64_t t0 = cabe::util::GetWallTimeNs();
    cabe::Engine engine;
    ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);
    const char* keys[] = {"alpha", "beta", "gamma"};
    const std::byte fills[] = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    for (int i = 0; i < 3; ++i) {
        const auto value = MakeValue(fills[i]);
        ASSERT_EQ(engine.Put(keys[i], cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        expects.push_back({keys[i], cabe::util::CRC32(cabe::DataView{value.data(), value.size()})});
    }
    const std::uint64_t t1 = cabe::util::GetWallTimeNs();
    ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);
    engine.Close();

    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    const cabe::SnapshotSlotHeader h = ReadHeader(a_off);
    ASSERT_TRUE(cabe::VerifySlotHeader(h));
    ASSERT_EQ(h.entry_count, 3u);

    // 读数据区、逐条解码
    const std::size_t aligned = cabe::util::AlignUp(3 * cabe::kSnapshotRecordSize, cabe::kSnapshotBlockSize);
    cabe::RawDevice dev;
    ASSERT_EQ(dev.Open(snap_), cabe::err::kSuccess);
    std::byte* buf = cabe::RawDevice::AllocAligned(aligned);
    ASSERT_NE(buf, nullptr);
    ASSERT_EQ(dev.ReadAt(a_off + cabe::kSnapshotSlotHeaderSize, buf, aligned), cabe::err::kSuccess);
    dev.Close();

    std::set<std::uint64_t> blocks;
    int matched = 0;
    for (int i = 0; i < 3; ++i) {
        cabe::SnapshotRecord r{};
        std::memcpy(&r, buf + static_cast<std::size_t>(i) * cabe::kSnapshotRecordSize, sizeof r);
        const std::string key(reinterpret_cast<const char*>(r.key), r.key_len);

        for (std::size_t j = r.key_len; j < cabe::kSnapshotKeyMax; ++j) {
            EXPECT_EQ(r.key[j], 0u) << "key 区尾部未补零 @" << j;
        }
        EXPECT_EQ(r.state, static_cast<std::uint8_t>(cabe::ValueState::Active));
        EXPECT_GE(r.timestamp, t0);   // Put 发生的时间窗
        EXPECT_LE(r.timestamp, t1);
        blocks.insert(r.block);

        bool found = false;
        for (const auto& e : expects) {
            if (e.key == key) {
                EXPECT_EQ(r.value_crc, e.value_crc) << "key=" << key;
                found = true;
                ++matched;
                break;
            }
        }
        EXPECT_TRUE(found) << "盘上出现未知 key: " << key;
    }
    EXPECT_EQ(matched, 3);
    EXPECT_EQ(blocks.size(), 3u);   // 三个活键各占一个不同的数据块
    cabe::RawDevice::FreeAligned(buf);
}

// 部署期容量校验拒绝：单槽装不下 block_count×128 → Open 返回 kDeviceTooSmall。
// 不造小设备（RawDevice 仅支持块设备/BLKGETSIZE64），改为伪造 block_count 巨大的数据设备
// 超级块，让单槽需求必然超过现有快照设备的半区——打的是同一条 slot_size < need 拒绝分支。
TEST_F(SnapshotDeviceTest, OpenRejectsSmallDevice) {
    cabe::Options opts;            // Snapshot::Open 只持指针读 snapshot_buffer_size，默认值即可
    cabe::SuperBlock sb{};
    sb.block_count = 1ull << 40;   // 需求 ≈ 2^40 × 128 字节，远超任何测试设备

    cabe::Snapshot snap;
    EXPECT_EQ(snap.Open(snap_, &opts, sb), cabe::err::kDeviceTooSmall);
    EXPECT_FALSE(snap.is_open());  // 拒开后设备句柄已关闭
}

// ============================================================
// P5M6：快照恢复侧（设计稿 doc/P5/P5M6_recovery_design.md §5/§13）
// 手法二（Close 后盘面篡改雕崩溃形态）+ 手法三（direct-Snapshot 直驱裁决/加载）。
// ============================================================

namespace {

// 读数据设备超级块（recover 校验路径，与 Engine 同源）——direct-Snapshot 的 Open 需要它。
cabe::SuperBlock RecoverDataSB(const std::string& data, const std::string& wal,
                               const std::string& snap) {
    cabe::DeviceConfig cfg;
    cfg.data_path = data; cfg.wal_path = wal; cfg.snapshot_path = snap;
    cabe::SuperBlock sb{};
    EXPECT_EQ(cabe::RecoverDeviceGroup(cfg, 0, &sb), cabe::err::kSuccess);
    return sb;
}

// 把 4K 原始块写到快照设备指定偏移（伪造槽头/篡改数据用）。
void WriteRaw4K(const std::string& snap_path, std::uint64_t off, const std::byte* src) {
    cabe::RawDevice dev;
    ASSERT_EQ(dev.Open(snap_path), cabe::err::kSuccess);
    std::byte* buf = cabe::RawDevice::AllocAligned(4096);
    ASSERT_NE(buf, nullptr);
    std::memcpy(buf, src, 4096);
    ASSERT_EQ(dev.WriteAt(off, buf, 4096), cabe::err::kSuccess);
    cabe::RawDevice::FreeAligned(buf);
    dev.Close();
}

// 翻转某槽数据区首块的一个字节（模拟腐烂；槽头保持合法）。
void FlipSlotDataByte(const std::string& snap_path, std::uint64_t slot_off) {
    cabe::RawDevice dev;
    ASSERT_EQ(dev.Open(snap_path), cabe::err::kSuccess);
    std::byte* buf = cabe::RawDevice::AllocAligned(4096);
    ASSERT_NE(buf, nullptr);
    const std::uint64_t off = slot_off + cabe::kSnapshotSlotHeaderSize;
    ASSERT_EQ(dev.ReadAt(off, buf, 4096), cabe::err::kSuccess);
    buf[8] = buf[8] ^ std::byte{0xFF};
    ASSERT_EQ(dev.WriteAt(off, buf, 4096), cabe::err::kSuccess);
    cabe::RawDevice::FreeAligned(buf);
    dev.Close();
}

// 把某槽头清零（模拟撕裂/作废）。
void ZeroSlotHeader(const std::string& snap_path, std::uint64_t slot_off) {
    std::byte zeros[4096] = {};
    WriteRaw4K(snap_path, slot_off, zeros);
}

// 空遍历器（写"0 条记录"的快照用）。
int32_t EmptyScan(const cabe::MetaIndexVisitor&) { return cabe::err::kSuccess; }

// 收集 Load 投递的 (key, meta)。
struct LoadedRec { std::string key; cabe::ValueMeta meta; };
cabe::MetaIndexVisitor CollectLoad(std::vector<LoadedRec>* out) {
    return [out](std::string_view key, const cabe::ValueMeta& m) -> int32_t {
        out->push_back({std::string(key), m});
        return cabe::err::kSuccess;
    };
}

} // namespace

// 双合法槽取代际最大；状态从盘上重建（active=B、next_gen=3）；Load 投递完整内容（2.2/2.5）。
TEST_F(SnapshotDeviceTest, RecoverPicksMaxGeneration) {
    const auto value = MakeValue(std::byte{0x11});
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k1", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen1 → A（含 k1）
        ASSERT_EQ(engine.Put("k2", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen2 → B（含 k1,k2）
        engine.Close();
    }
    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    const cabe::SnapshotSlotHeader hb = ReadHeader(b_off);

    const cabe::SuperBlock sb = RecoverDataSB(data_, wal_, snap_);
    cabe::Options ropts;                                            // create=false（默认）
    cabe::Snapshot snap;
    ASSERT_EQ(snap.Open(snap_, &ropts, sb), cabe::err::kSuccess);
    EXPECT_EQ(snap.last_covered_seq(), hb.covered_seq) << "选中代际最大的 B";

    std::vector<LoadedRec> recs;
    ASSERT_EQ(snap.Load(CollectLoad(&recs)), cabe::err::kSuccess);
    ASSERT_EQ(recs.size(), 2u);

    // next_gen = max+1 = 3，写非活跃槽 = A（active=B）——下一份快照直接验证状态重建。
    ASSERT_EQ(snap.Write(snap.last_covered_seq(), EmptyScan), cabe::err::kSuccess);
    const cabe::SnapshotSlotHeader ha = ReadHeader(a_off);
    ASSERT_TRUE(cabe::VerifySlotHeader(ha));
    EXPECT_EQ(ha.generation, 3u);
    snap.Close();
}

// 单合法槽（另一槽撕裂/清零）→ 取之（2.2 单合法形态——A/B 双缓冲的设计场景）。
TEST_F(SnapshotDeviceTest, RecoverSingleValidSlot) {
    const auto value = MakeValue(std::byte{0x22});
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k1", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen1 → A
        ASSERT_EQ(engine.Put("k2", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen2 → B
        engine.Close();
    }
    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    const cabe::SnapshotSlotHeader ha = ReadHeader(a_off);
    ZeroSlotHeader(snap_, b_off);                                   // B 头作废（模拟撕裂）

    const cabe::SuperBlock sb = RecoverDataSB(data_, wal_, snap_);
    cabe::Options ropts;
    cabe::Snapshot snap;
    ASSERT_EQ(snap.Open(snap_, &ropts, sb), cabe::err::kSuccess);
    EXPECT_EQ(snap.last_covered_seq(), ha.covered_seq) << "回到唯一合法的 A（gen1）";

    std::vector<LoadedRec> recs;
    ASSERT_EQ(snap.Load(CollectLoad(&recs)), cabe::err::kSuccess);
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_EQ(recs[0].key, "k1");

    // next_gen = max(合法头)=1 → 2；写非活跃槽 = B。
    ASSERT_EQ(snap.Write(snap.last_covered_seq(), EmptyScan), cabe::err::kSuccess);
    const cabe::SnapshotSlotHeader hb = ReadHeader(b_off);
    ASSERT_TRUE(cabe::VerifySlotHeader(hb));
    EXPECT_EQ(hb.generation, 2u);
    snap.Close();
}

// 双槽头皆无效 = 合法的"从未快照"（covered=0，Load 空跑；非错误，2.5）。
TEST_F(SnapshotDeviceTest, RecoverNoSnapshot) {
    const auto value = MakeValue(std::byte{0x33});
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);   // create 清两槽
        ASSERT_EQ(engine.Put("k1", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        engine.Close();                                             // 没做过快照
    }
    const cabe::SuperBlock sb = RecoverDataSB(data_, wal_, snap_);
    cabe::Options ropts;
    cabe::Snapshot snap;
    ASSERT_EQ(snap.Open(snap_, &ropts, sb), cabe::err::kSuccess);
    EXPECT_EQ(snap.last_covered_seq(), 0u);

    std::vector<LoadedRec> recs;
    ASSERT_EQ(snap.Load(CollectLoad(&recs)), cabe::err::kSuccess);  // 空跑成功
    EXPECT_TRUE(recs.empty());

    // next_gen=1、active=-1 → 首份快照写 A、代际 1（与 create 后首写一致）。
    ASSERT_EQ(snap.Write(0, EmptyScan), cabe::err::kSuccess);
    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    const cabe::SnapshotSlotHeader ha = ReadHeader(a_off);
    ASSERT_TRUE(cabe::VerifySlotHeader(ha));
    EXPECT_EQ(ha.generation, 1u);
    snap.Close();
}

// 坏槽回退（2.3）：候选 gen2 数据腐烂 → 回退 gen1；next_gen = 盘上合法头 max+1 = 3（2.5 尸体论证）。
TEST_F(SnapshotDeviceTest, RecoverFallbackOnDataCrc) {
    const auto value = MakeValue(std::byte{0x44});
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k1", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen1 → A（k1）
        ASSERT_EQ(engine.Put("k2", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen2 → B（k1,k2）
        engine.Close();
    }
    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    const cabe::SnapshotSlotHeader ha = ReadHeader(a_off);
    FlipSlotDataByte(snap_, b_off);                                 // B 数据腐烂（头合法）

    const cabe::SuperBlock sb = RecoverDataSB(data_, wal_, snap_);
    cabe::Options ropts;
    cabe::Snapshot snap;
    ASSERT_EQ(snap.Open(snap_, &ropts, sb), cabe::err::kSuccess) << "回退应成功";
    EXPECT_EQ(snap.last_covered_seq(), ha.covered_seq) << "回退到 gen1 的覆盖点";

    std::vector<LoadedRec> recs;
    ASSERT_EQ(snap.Load(CollectLoad(&recs)), cabe::err::kSuccess);
    ASSERT_EQ(recs.size(), 1u) << "加载的是 gen1 内容";
    EXPECT_EQ(recs[0].key, "k1");

    // 关键断言：续号必须压过盘上残存的 gen2 尸体——next_gen = max(1,2)+1 = 3，
    // 写非活跃槽 = B（坏者被覆盖，好者作后盾）。
    ASSERT_EQ(snap.Write(snap.last_covered_seq(), EmptyScan), cabe::err::kSuccess);
    const cabe::SnapshotSlotHeader hb = ReadHeader(b_off);
    ASSERT_TRUE(cabe::VerifySlotHeader(hb));
    EXPECT_EQ(hb.generation, 3u) << "\"选中槽+1\"=2 会让新快照永远输给尸体";
    snap.Close();
}

// 双槽数据皆坏 → 拒开不降级（2.3 梯子终点：合法头证明快照存在过，假装从未快照是编造历史）。
TEST_F(SnapshotDeviceTest, RecoverBothDataBad) {
    const auto value = MakeValue(std::byte{0x55});
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k1", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen1 → A
        ASSERT_EQ(engine.Put("k2", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen2 → B
        engine.Close();
    }
    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    FlipSlotDataByte(snap_, a_off);
    FlipSlotDataByte(snap_, b_off);

    const cabe::SuperBlock sb = RecoverDataSB(data_, wal_, snap_);
    cabe::Options ropts;
    cabe::Snapshot snap;
    EXPECT_EQ(snap.Open(snap_, &ropts, sb), cabe::err::kSnapshotCorrupted);
    EXPECT_FALSE(snap.is_open());
}

// 双合法撞代际 → 拒开（2.2：写侧构造保证不撞号，相等即证据矛盾）。
TEST_F(SnapshotDeviceTest, RecoverGenerationTie) {
    const auto value = MakeValue(std::byte{0x66});
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k1", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen1 → A
        engine.Close();
    }
    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    const cabe::SnapshotSlotHeader ha = ReadHeader(a_off);          // 伪造：B = A 的逐字节拷贝
    std::byte raw[4096];
    std::memcpy(raw, &ha, sizeof ha);
    WriteRaw4K(snap_, b_off, raw);

    const cabe::SuperBlock sb = RecoverDataSB(data_, wal_, snap_);
    cabe::Options ropts;
    cabe::Snapshot snap;
    EXPECT_EQ(snap.Open(snap_, &ropts, sb), cabe::err::kSnapshotCorrupted);
    EXPECT_FALSE(snap.is_open());
}

// Load 往返：盘上记录解码回来与 Put 内容逐字段等值；含空快照（entry_count=0 合法，2.4）。
TEST_F(SnapshotDeviceTest, LoadRoundTrip) {
    struct Expect { std::string key; std::uint32_t value_crc; };
    std::vector<Expect> expects;
    const std::uint64_t t0 = cabe::util::GetWallTimeNs();
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);
        const char* keys[] = {"alpha", "beta", "gamma"};
        const std::byte fills[] = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
        for (int i = 0; i < 3; ++i) {
            const auto value = MakeValue(fills[i]);
            ASSERT_EQ(engine.Put(keys[i], cabe::DataView{value.data(), value.size()}).code,
                      cabe::err::kSuccess);
            expects.push_back({keys[i], cabe::util::CRC32(cabe::DataView{value.data(), value.size()})});
        }
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);
        engine.Close();
    }
    const std::uint64_t t1 = cabe::util::GetWallTimeNs();

    const cabe::SuperBlock sb = RecoverDataSB(data_, wal_, snap_);
    cabe::Options ropts;
    cabe::Snapshot snap;
    ASSERT_EQ(snap.Open(snap_, &ropts, sb), cabe::err::kSuccess);
    std::vector<LoadedRec> recs;
    ASSERT_EQ(snap.Load(CollectLoad(&recs)), cabe::err::kSuccess);
    snap.Close();

    ASSERT_EQ(recs.size(), 3u);
    std::set<std::uint64_t> blocks;
    int matched = 0;
    for (const auto& r : recs) {
        EXPECT_EQ(r.meta.state, cabe::ValueState::Active);
        EXPECT_EQ(r.meta.block.dev(), 0);
        EXPECT_GE(r.meta.timestamp, t0);
        EXPECT_LE(r.meta.timestamp, t1);
        blocks.insert(r.meta.block.block_idx());
        for (const auto& e : expects) {
            if (e.key == r.key) { EXPECT_EQ(r.meta.crc, e.value_crc) << r.key; ++matched; break; }
        }
    }
    EXPECT_EQ(matched, 3);
    EXPECT_EQ(blocks.size(), 3u);

    // 空快照：索引为空时做快照（entry_count=0、合法）→ 恢复选中它、Load 空跑成功。
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);   // 重新 create（清两槽）
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);         // gen1：0 条记录
        engine.Close();
    }
    const cabe::SuperBlock sb2 = RecoverDataSB(data_, wal_, snap_);
    cabe::Snapshot snap2;
    ASSERT_EQ(snap2.Open(snap_, &ropts, sb2), cabe::err::kSuccess);
    std::vector<LoadedRec> recs2;
    ASSERT_EQ(snap2.Load(CollectLoad(&recs2)), cabe::err::kSuccess);
    EXPECT_TRUE(recs2.empty());
    // 空快照被当作活跃槽（≠ 无快照）：下一份写非活跃槽 B、代际 2。
    ASSERT_EQ(snap2.Write(0, EmptyScan), cabe::err::kSuccess);
    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);
    EXPECT_EQ(ReadHeader(b_off).generation, 2u);
    snap2.Close();
}

// Load 的内存安全守卫：key_len > 字段容量（96）的记录 → kSnapshotCorrupted（2.4；
// data_crc 同步伪造使 Open 裁决通过——专打"CRC 过了但内容非法"这一类）。
TEST_F(SnapshotDeviceTest, LoadRejectsOversizeKeyLen) {
    const auto value = MakeValue(std::byte{0x77});
    {
        cabe::Engine engine;
        ASSERT_EQ(engine.Open(MakeOpts()).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Put("k1", cabe::DataView{value.data(), value.size()}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine.Snapshot().code, cabe::err::kSuccess);     // gen1 → A，1 条记录
        engine.Close();
    }
    std::uint64_t a_off = 0, b_off = 0;
    Layout(&a_off, &b_off);

    // 伪造：记录 0 的 key_len 改成 200，重算 data_crc + 槽头 CRC（让 Open 全部通过）。
    cabe::RawDevice dev;
    ASSERT_EQ(dev.Open(snap_), cabe::err::kSuccess);
    std::byte* buf = cabe::RawDevice::AllocAligned(4096);
    ASSERT_NE(buf, nullptr);
    ASSERT_EQ(dev.ReadAt(a_off + cabe::kSnapshotSlotHeaderSize, buf, 4096), cabe::err::kSuccess);
    cabe::SnapshotRecord rec{};
    std::memcpy(&rec, buf, sizeof rec);
    rec.key_len = 200;                                              // > kSnapshotKeyMax(96)
    std::memcpy(buf, &rec, sizeof rec);
    ASSERT_EQ(dev.WriteAt(a_off + cabe::kSnapshotSlotHeaderSize, buf, 4096), cabe::err::kSuccess);

    cabe::SnapshotSlotHeader h{};
    std::byte* hb = cabe::RawDevice::AllocAligned(4096);
    ASSERT_NE(hb, nullptr);
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

    const cabe::SuperBlock sb = RecoverDataSB(data_, wal_, snap_);
    cabe::Options ropts;
    cabe::Snapshot snap;
    ASSERT_EQ(snap.Open(snap_, &ropts, sb), cabe::err::kSuccess) << "伪造的 data_crc 应让裁决通过";
    std::vector<LoadedRec> recs;
    EXPECT_EQ(snap.Load(CollectLoad(&recs)), cabe::err::kSnapshotCorrupted);
    snap.Close();
}
