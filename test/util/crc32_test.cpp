#include "util/crc32.h"
#include "util/cpu_features.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {
    cabe::DataView AsBytes(std::string_view s) {
        return std::as_bytes(std::span{s.data(), s.size()});
    }
} // namespace

TEST(Crc32, KnownVector) {
    EXPECT_EQ(cabe::util::CRC32(AsBytes("123456789")), 0xE3069283u);
}

TEST(Crc32, EmptyBufferIsZero) {
    EXPECT_EQ(cabe::util::CRC32(cabe::DataView{}), 0u);
}

TEST(Crc32, SoftwareHardwareConsistency) {
#if defined(__x86_64__) || defined(__i386__)
    if (!cabe::util::cpu::HasSSE42()) {
        GTEST_SKIP() << "CPU 无 SSE4.2，跳过硬件路径";
    }
    std::array<std::byte, 1000> buf{};
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<std::byte>((i * 31 + 7) & 0xFF);
    }
    const cabe::DataView dv{buf};
    const auto sw = cabe::util::detail::SoftwareCRC32C(dv);
    const auto hw = cabe::util::detail::HardwareCRC32C_x86(dv);
    EXPECT_EQ(sw, hw);
    EXPECT_EQ(sw, cabe::util::CRC32(dv));
#else
    GTEST_SKIP() << "非 x86，无硬件路径";
#endif
}
