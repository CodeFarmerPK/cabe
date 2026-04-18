/*
 * Project: Cabe
 * Created Time: 2025-05-16 14:51
 * Created by: CodeFarmerPK
 *
 * CRC32C (Castagnoli polynomial 0x1EDC6F41) with runtime CPU dispatch:
 *   - Hardware:  SSE4.2  _mm_crc32_u64  / _mm_crc32_u8
 *   - Software:  constexpr-generated 256-entry table, byte-at-a-time
 *
 * Algorithm choice: CRC32C, not IEEE CRC32, because:
 *   1. Hardware-accelerated on every x86_64 CPU since Nehalem (2008)
 *      and on ARMv8.1 (future ARM port path).
 *   2. Industry standard (ext4 / btrfs / ZFS / RocksDB / ScyllaDB).
 *   3. Better burst-error detection on long messages.
 *
 * Thread safety:
 *   The dispatcher pointer is initialized once at static-init time
 *   and is `const` after that. All subsequent reads are lock-free.
 */

#include "crc32.h"
#include "cpu_features.h"

#include <array>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#  include <nmmintrin.h>
#endif

namespace cabe::util {
    namespace {

        // ---------- 软件 fallback ----------
        constexpr std::array<uint32_t, 256> MakeCRC32CTable() {
            constexpr uint32_t kRevPoly = 0x82F63B78u;
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int j = 0; j < 8; ++j) c = (c >> 1) ^ ((c & 1u) ? kRevPoly : 0u);
                t[i] = c;
            }
            return t;
        }
        constexpr auto kCRC32CTable = MakeCRC32CTable();

        uint32_t SoftwareCRC32C(DataView data) noexcept {
            uint32_t crc = 0xFFFFFFFFu;
            for (const char c : data) {
                crc = (crc >> 8) ^ kCRC32CTable[(crc ^ static_cast<uint8_t>(c)) & 0xFFu];
            }
            return ~crc;
        }

        // ---------- x86 硬件路径 ----------
#if defined(__x86_64__) || defined(__i386__)
        [[gnu::target("sse4.2")]]
        uint32_t HardwareCRC32C_x86(DataView data) noexcept {
            uint64_t crc = 0xFFFFFFFFu;
            const char* p = data.data();
            size_t len   = data.size();
            while (len >= 8) {
                uint64_t chunk;
                std::memcpy(&chunk, p, sizeof(chunk));
                crc = _mm_crc32_u64(crc, chunk);
                p += 8; len -= 8;
            }
            auto crc32 = static_cast<uint32_t>(crc);
            while (len-- > 0) crc32 = _mm_crc32_u8(crc32, static_cast<uint8_t>(*p++));
            return ~crc32;
        }
#endif

        // ---------- 未来 ARM 路径（预留占位，当前不启用）----------
        // #if defined(__aarch64__)
        // uint32_t HardwareCRC32C_ARM(DataView data) noexcept { ... __crc32cd ... }
        // #endif

        // ---------- 运行时 dispatch ----------
        using CRCFunc = uint32_t (*)(DataView) noexcept;

        CRCFunc SelectImpl() noexcept {
#if defined(__x86_64__) || defined(__i386__)
            if (cpu::HasSSE42()) return HardwareCRC32C_x86;
#endif
            // if (cpu::HasARMCRC()) return HardwareCRC32C_ARM;   // 未来
            return SoftwareCRC32C;
        }

        const CRCFunc kCRC32Impl = SelectImpl();

    } // namespace

    uint32_t CRC32(const DataView data) {
        return kCRC32Impl(data);
    }

} // namespace cabe::util