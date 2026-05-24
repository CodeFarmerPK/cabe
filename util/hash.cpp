/*
 * Project: Cabe
 * Created Time: 2026-05-22
 * Created by: CodeFarmerPK
 *
 * 路由 hash 实现（XXH3 64-bit）。
 * XXH_INLINE_ALL：xxhash 全部内联进本 TU，无需链接 libxxhash；xxhash 符号也只限于本 TU。
 */
#define XXH_INLINE_ALL
#include "third_party/xxhash/xxhash.h"

#include "hash.h"

#include <cassert>
#include <cstdlib>   // std::abort

namespace cabe::util {

    std::uint64_t Hash(DataView data) noexcept {
        return XXH3_64bits(data.data(), data.size()); // seed 0，固定（D6 冻结）
    }

    std::uint64_t Hash(std::string_view key) noexcept {
        return XXH3_64bits(key.data(), key.size());
    }

    DeviceId RouteToDevice(std::string_view key, std::size_t n_devices) noexcept {
        assert(n_devices >= 1 && n_devices <= 256 && "n_devices must be in [1,256]"); // Debug 防线
        // Release 兜底：Debug assert 在 NDEBUG 下被消除；n_devices==0 会让下行 Hash%0 触发
        // 整数除零（x86 上 SIGFPE），属 UB 路径。无条件 abort 守护，不让 UB 进生产。
        if (n_devices == 0) std::abort();
        return static_cast<DeviceId>(Hash(key) % n_devices);                           // D7
    }

} // namespace cabe::util
