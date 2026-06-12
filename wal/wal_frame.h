#ifndef CABE_WAL_FRAME_H
#define CABE_WAL_FRAME_H

// P5M2：WAL 帧的盘上格式（128 字节固定帧）。
// 设计依据：doc/P5/P5M2_wal_core_design.md §4。
// 仅 Linux/x86_64：native 小端 memcpy，不做跨端序列化（与 SuperBlock 一致）。

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cabe {

    inline constexpr std::uint32_t kWalMagic        = 0x45424143u; // "CABE"
    inline constexpr std::uint32_t kWalFrameVersion = 1;           // 帧格式版本（非快照代际）
    inline constexpr std::size_t   kWalFrameSize    = 128;         // 单帧 128 字节
    inline constexpr std::size_t   kWalKeyMax       = 84;          // key 最大字节数（超出 Put 拒绝）
    inline constexpr std::size_t   kWalBlockSize    = 4096;        // 4K：O_DIRECT 持久写单位
    inline constexpr std::size_t   kWalFramesPerBlock = kWalBlockSize / kWalFrameSize; // 32 帧/块

    static_assert(kWalBlockSize % kWalFrameSize == 0, "帧必须整除 4K 块（不跨块）");

    enum class WalEntryType : std::uint8_t {
        Put    = 1,
        Delete = 2,
        // 未来可扩展（如 checkpoint）：每帧单一类型，新增取值不破坏布局。
    };

    // 统一 128 字节帧。Delete（墓碑）复用本结构，block / value_crc 填 0。
    // 字段顺序经排布使 8 对齐字段（seq/block/timestamp）落在 8 的倍数偏移，无 padding。
    struct WalFrame {
        std::uint32_t magic;        // @0    kWalMagic
        std::uint8_t  version;      // @4    kWalFrameVersion
        std::uint8_t  flags;        // @5    预留标志位（M2 不用）
        std::uint8_t  entry_type;   // @6    WalEntryType
        std::uint8_t  reserved0;    // @7    对齐/预留
        std::uint64_t seq;          // @8    单调 LSN（排序 + 环形区消歧 + 恢复重放定边界，P5M6 兑现）
        std::uint64_t block;        // @16   BlockId.raw（Delete 填 0）
        std::uint64_t timestamp;    // @24   util::GetWallTimeNs（还原 ValueMeta.timestamp / 未来 TTL）
        std::uint32_t value_crc;    // @32   value 的 CRC32C（Delete 填 0）
        std::uint16_t key_len;      // @36   实际 key 字节数（≤ kWalKeyMax）
        std::uint16_t reserved1;    // @38   对齐/预留
        std::uint8_t  key[84];      // @40   key，尾部补零
        std::uint32_t frame_crc32c; // @124  帧自身 CRC32C，覆盖 [0, 124)
    };

    static_assert(sizeof(WalFrame) == kWalFrameSize);
    static_assert(std::is_standard_layout_v<WalFrame>);
    static_assert(std::is_trivially_copyable_v<WalFrame>);

} // namespace cabe

#endif // CABE_WAL_FRAME_H
