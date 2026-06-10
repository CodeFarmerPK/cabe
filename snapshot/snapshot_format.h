#ifndef CABE_SNAPSHOT_FORMAT_H
#define CABE_SNAPSHOT_FORMAT_H

// P5M4：快照盘上格式（设计依据：doc/P5/P5M4_snapshot_design.md §4）。
// 仅 Linux/x86_64：native 小端 memcpy，不做跨端序列化（与 SuperBlock / WalFrame 一致）。
//
// 两个结构：
//   SnapshotRecord     —— 128 字节定长，一条 = 内存索引里一个活键的索引项（key + ValueMeta）。
//   SnapshotSlotHeader —— 4096 字节，一份快照的元信息（代际 / covered_seq / 双 CRC 之一）。
// 每条记录不带 CRC（整份一条 data_crc 在槽头）；记录也不带 magic/version（槽头记一次）。

#include "common/structs.h"   // ValueMeta / BlockId / DataView
#include "util/crc32.h"       // CRC32
#include "wal/wal_frame.h"    // kWalKeyMax（仅用其编译期常量做 static_assert）

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace cabe {

    inline constexpr std::size_t   kSnapshotRecordSize     = 128;   // 128 整除 4096 → 32 条/4K 块、不跨页
    inline constexpr std::size_t   kSnapshotKeyMax         = 96;    // 必须 ≥ kWalKeyMax(84)；留头
    inline constexpr std::size_t   kSnapshotBlockSize      = 4096;  // 4K：O_DIRECT 持久写单位
    inline constexpr std::size_t   kSnapshotSlotHeaderSize = 4096;  // 槽头独占一个 4K 块

    static_assert(kSnapshotKeyMax >= kWalKeyMax, "快照 key 区必须能装下任何合法 key（Put 已卡 kWalKeyMax）");
    static_assert(kSnapshotBlockSize % kSnapshotRecordSize == 0, "记录必须整除 4K 块（不跨页）");

    inline constexpr std::uint32_t kSnapshotSlotMagic   = 0x50414E53u; // "SNAP"（小端）
    inline constexpr std::uint32_t kSnapshotSlotVersion = 1;

    // 由快照设备字节数算单槽大小：可用区（8K 之后）对半切、向下取整 4K。
    // "向下取整"身兼两职：两槽都装得下（不冲出设备尾）+ B 槽起点（A 起点 + 槽大小）4K 对齐。
    // 设备过小（≤8K）返回 0。Snapshot::Open 与测试共用——槽布局公式只此一处（单一来源）。
    inline constexpr std::uint64_t SnapshotSlotSize(std::uint64_t device_bytes) noexcept {
        return device_bytes <= kDataRegionOffset
                   ? 0
                   : ((device_bytes - kDataRegionOffset) / 2 / kSnapshotBlockSize) * kSnapshotBlockSize;
    }

    // ---- 单条记录（128 字节定长）----
    // 字段经排布使 8 对齐字段（block/timestamp）落在 8 的倍数偏移，无 padding。
    struct SnapshotRecord {
        std::uint16_t key_len;      // @0    实际 key 字节数（≤ kWalKeyMax）
        std::uint8_t  state;        // @2    ValueState（恒 Active；留位以备软删除入快照）
        std::uint8_t  reserved0;    // @3
        std::uint32_t value_crc;    // @4    value 的 CRC32C（来自 ValueMeta.crc）
        std::uint64_t block;        // @8    BlockId.raw
        std::uint64_t timestamp;    // @16   ValueMeta.timestamp
        std::uint8_t  key[96];      // @24   key，尾部补零
        std::uint8_t  reserved1[8]; // @120  预留扩展
    };
    static_assert(sizeof(SnapshotRecord) == kSnapshotRecordSize);
    static_assert(std::is_standard_layout_v<SnapshotRecord>);
    static_assert(std::is_trivially_copyable_v<SnapshotRecord>);

    // ---- 槽头（4096 字节，对标 SuperBlock：字段 + 大段 reserved + 末尾自身 CRC）----
    struct SnapshotSlotHeader {
        std::uint32_t magic;          // @0     kSnapshotSlotMagic
        std::uint32_t version;        // @4     槽头格式版本
        std::uint64_t generation;     // @8     代际号（单调递增；比大小定最新）
        std::uint64_t covered_seq;    // @16    覆盖到的最大 WAL seq（M5 回收 / M6 重放起点）
        std::uint64_t entry_count;    // @24    记录条数
        std::uint64_t data_len;       // @32    记录区字节长度（= entry_count × 128，交叉校验）
        std::uint32_t data_crc;       // @40    记录流 CRC32C（覆盖 entry_count × 128 字节）
        std::uint32_t reserved0;      // @44    对齐
        std::uint64_t created_at;     // @48    生成时间戳（util::GetWallTimeNs，诊断用）
        std::uint8_t  engine_uuid[16];// @56    引擎 UUID（与超级块比对，挡前朝残留槽）
        std::uint8_t  reserved[4020]; // @72    预留扩展，填零
        std::uint32_t header_crc32c;  // @4092  槽头自身 CRC32C，覆盖 [0, 4092)
    };
    static_assert(sizeof(SnapshotSlotHeader) == kSnapshotSlotHeaderSize);
    static_assert(std::is_standard_layout_v<SnapshotSlotHeader>);
    static_assert(std::is_trivially_copyable_v<SnapshotSlotHeader>);

    // 把一个活键索引项编码成一条记录（零初始化保证 key 补零 + reserved 清零）。
    inline SnapshotRecord EncodeSnapshotRecord(std::string_view key, const ValueMeta& meta) {
        SnapshotRecord r{};
        r.key_len   = static_cast<std::uint16_t>(key.size());
        r.state     = static_cast<std::uint8_t>(meta.state);
        r.value_crc = meta.crc;
        r.block     = meta.block.raw;
        r.timestamp = meta.timestamp;
        if (!key.empty()) {
            std::memcpy(r.key, key.data(), key.size());   // 调用方保证 ≤ kSnapshotKeyMax
        }
        return r;
    }

    // 槽头自身 CRC：覆盖 [0, 4092)（整块去掉末尾 4 字节的 header_crc32c 本身）。
    inline std::uint32_t ComputeSlotHeaderCrc(const SnapshotSlotHeader& h) {
        const auto* p = reinterpret_cast<const std::byte*>(&h);
        return util::CRC32(DataView{p, kSnapshotSlotHeaderSize - sizeof h.header_crc32c});
    }

    // 校验槽头：魔数 / 版本 / 自身 CRC。供加载（M6）与测试使用。
    inline bool VerifySlotHeader(const SnapshotSlotHeader& h) {
        if (h.magic != kSnapshotSlotMagic) return false;
        if (h.version != kSnapshotSlotVersion) return false;
        return ComputeSlotHeaderCrc(h) == h.header_crc32c;
    }

} // namespace cabe

#endif // CABE_SNAPSHOT_FORMAT_H
