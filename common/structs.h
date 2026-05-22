/*
* Project: Cabe
 * Created Time: 2025-05-16 17:41
 * Created by: CodeFarmerPK
 */
#ifndef CABE_STRUCTS_H
#define CABE_STRUCTS_H

// Cabe 当前仅支持 Linux（依赖 O_DIRECT / mmap / pread / pwrite / liburing）
// CMake 已在配置阶段做同样检查，此处为源码级兜底，防止跨平台 IDE 误触发构建
#if !defined(__linux__)
#  error "Cabe currently only supports Linux (target: Fedora 43). See README.md."
#endif

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace cabe {
    // ---- 固定 value 大小（D1）----
    inline constexpr std::size_t kValueSize = 1024 * 1024; // 1 MiB

    // ---- 设备标识（D5：占 BlockId 高 8 位）----
    using DeviceId = std::uint8_t;

    // ---- 数据视图（D2/D4：设备上只有裸字节）----
    using DataView = std::span<const std::byte>;
    using DataBuffer = std::span<std::byte>;

    // ---- 物理寻址（D5：device_id:8 | block_idx:56）----
    struct BlockId {
        std::uint64_t raw;

        static constexpr std::uint64_t kIdxBits = 56;
        static constexpr std::uint64_t kIdxMask = (std::uint64_t{1} << kIdxBits) - 1;

        static constexpr BlockId Make(DeviceId dev, std::uint64_t block_idx) noexcept {
            assert(block_idx <= kIdxMask && "block_idx exceeds 56 bits"); // Debug 防线；NDEBUG 下消除
            return BlockId{(static_cast<std::uint64_t>(dev) << kIdxBits) | (block_idx & kIdxMask)};
        }

        constexpr DeviceId dev() const noexcept { return static_cast<DeviceId>(raw >> kIdxBits); }
        constexpr std::uint64_t block_idx() const noexcept { return raw & kIdxMask; }
        constexpr std::uint64_t byte_offset() const noexcept { return block_idx() * kValueSize; } // 块号 × 块大小

        // C++20：defaulted <=> 自动合成 == 与全部关系运算（FreeList 排序 / 一致性比较用）
        constexpr auto operator<=>(const BlockId &) const noexcept = default;
    };

    static_assert(sizeof(BlockId) == 8);
    static_assert(std::is_trivially_copyable_v<BlockId>);

    // ---- value 状态（取代旧的数据状态枚举）----
    enum class ValueState : std::uint8_t {
        Active = 0,
        Deleted = 1,
    };

    // ---- value 元数据（D3：仅存在于 RAM / WAL）----
    // 字段顺序为达成 sizeof==24 而重排（block/timestamp/crc/state = 8/8/4/1）；末尾 reserved[3]
    // 把隐式 padding 显式化并清零，保证整体可确定地 memcpy 序列化（P5 WAL/snapshot）。
    struct ValueMeta {
        BlockId block{}; // 物理位置                       @0  (8)
        std::uint64_t timestamp{}; // 写入时间，util::GetWallTimeNs   @8  (8)
        std::uint32_t crc{}; // value 的 CRC32C（D14）          @16 (4)
        ValueState state = ValueState::Active; // Active / Deleted                @20 (1)
        std::uint8_t reserved[3] = {}; // 显式占位 + 预留小扩展位          @21 (3)
    };

    static_assert(sizeof(ValueMeta) == 24);
    static_assert(alignof(ValueMeta) == 8);
    static_assert(std::is_trivially_copyable_v<ValueMeta>);
    static_assert(std::is_standard_layout_v<ValueMeta>);

    // ---- WAL 帧头占位（D13；真实编解码在 P5，届时这些常量可迁入 wal 模块）----
    // 布局：magic:4 | version:1 | flags:1 | entry_type:1 | reserved:1 = 8 字节
    inline constexpr std::size_t kWalFrameHeaderSize = 8;
    inline constexpr std::uint32_t kWalMagic = 0x45424143u; // "CABE"（字节序在 P5 最终确定）
    inline constexpr std::uint8_t kWalVersion = 1;
    inline constexpr std::size_t kWalOffMagic = 0;
    inline constexpr std::size_t kWalOffVersion = 4;
    inline constexpr std::size_t kWalOffFlags = 5;
    inline constexpr std::size_t kWalOffEntryType = 6;
    inline constexpr std::size_t kWalOffReserved = 7;
} // namespace cabe

#endif // CABE_STRUCTS_H
