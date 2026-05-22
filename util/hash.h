/*
 * Project: Cabe
 * Created Time: 2026-05-22
 * Created by: CodeFarmerPK
 *
 * 路由 hash：xxh3 64-bit（D6，v2.0 前冻结）。
 * 仅用于 key → device 路由；数据完整性校验走 util/crc32 的 CRC32C（D14），二者不混用。
 */
#ifndef CABE_HASH_H
#define CABE_HASH_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "common/structs.h" // DataView, DeviceId

namespace cabe::util {

    // xxh3 64-bit，固定 seed 0，v2.0 前冻结（D6）。仅路由用，勿用于完整性校验。
    std::uint64_t Hash(DataView data) noexcept;
    std::uint64_t Hash(std::string_view key) noexcept;

    // D7：device_idx = Hash(key) % n_devices。前置 n_devices ∈ [1, 256]（DeviceId 为 uint8_t）。
    DeviceId RouteToDevice(std::string_view key, std::size_t n_devices) noexcept;

} // namespace cabe::util

#endif // CABE_HASH_H
