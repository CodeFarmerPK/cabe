#ifndef CABE_SUPER_BLOCK_H
#define CABE_SUPER_BLOCK_H

#include "engine/options.h"
#include "common/structs.h"   // kSuperBlockSize / kDataRegionOffset（布局常量）

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cabe {

    // kSuperBlockSize（4K）/ kDataRegionOffset（8K）在 common/structs.h 定义——io 与 engine 共享。
    inline constexpr std::uint32_t kSuperBlockMagic   = 0x43424553u; // "SEBC"
    inline constexpr std::uint32_t kSuperBlockVersion = 1;
    inline constexpr std::size_t   kUuidBytes         = 16;          // 128 位 UUID

    enum class DeviceType : std::uint32_t {
        Data     = 0,
        Wal      = 1,
        Snapshot = 2,
    };

    // 统一超级块结构（4K）。按 device_type 填充相关字段，未用字段留零。
    // standard-layout + trivially-copyable，可直接 memcpy 到 4K 对齐缓冲后写出。
    struct SuperBlock {
        std::uint32_t magic;                    // @0    kSuperBlockMagic
        std::uint32_t version;                  // @4    格式版本
        std::uint32_t device_type;              // @8    DeviceType
        std::uint32_t reserved0;                // @12   对齐
        std::uint8_t  engine_uuid[16];          // @16   引擎全局 UUID（三设备共享）
        std::uint8_t  device_uuid[16];          // @32   本设备唯一 UUID
        std::uint8_t  paired_data_uuid[16];     // @48   所属数据设备 UUID（WAL/快照填）
        std::uint8_t  paired_wal_uuid[16];      // @64   配对 WAL UUID（数据设备填）
        std::uint8_t  paired_snapshot_uuid[16]; // @80   配对快照 UUID（数据设备填）
        std::uint64_t device_id;                // @96   数据设备编号（多设备顺序校验，M1 下为 0）
        std::uint64_t block_count;              // @104  数据区可用块数=(设备字节-kDataRegionOffset)/kValueSize；仅数据设备有效，WAL/快照为 0
        std::uint64_t created_at;               // @112  创建时间戳
        std::uint8_t  reserved[3972];           // @120  预留扩展，填零
        std::uint32_t crc32c;                   // @4092 自身 CRC32C，覆盖 [0, 4092)
    };

    static_assert(sizeof(SuperBlock) == kSuperBlockSize);
    static_assert(std::is_standard_layout_v<SuperBlock>);
    static_assert(std::is_trivially_copyable_v<SuperBlock>);

    // 为一个设备组（数据 + WAL + 快照）写超级块（create，破坏性）。
    // 生成引擎 UUID + 三设备 UUID，按 device_type 填充，各写双份。
    // 返回写入数据设备的 SuperBlock（out）。
    int32_t CreateDeviceGroup(const DeviceConfig& cfg, std::uint64_t device_id, SuperBlock* out);

    // 校验一个设备组的超级块（recover）。
    // 读三设备超级块（主备 + 主坏用备份修复），校验 engine_uuid 一致 + 双向配对 + device_id。
    // 返回数据设备的 SuperBlock（out）。
    int32_t RecoverDeviceGroup(const DeviceConfig& cfg, std::uint64_t expected_device_id, SuperBlock* out);

} // namespace cabe

#endif // CABE_SUPER_BLOCK_H
