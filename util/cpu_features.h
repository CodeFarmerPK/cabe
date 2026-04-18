//
// Created by root on 4/18/26.
//

#ifndef CABE_CPU_FEATURES_H
#define CABE_CPU_FEATURES_H

namespace cabe::util::cpu {

    enum class Arch {
        Unknown,
        X86_64,
        AArch64,
    };

    Arch GetArch() noexcept;

    // ---- x86 capability queries ----
    // On non-x86 builds these always return false.
    bool HasSSE42() noexcept;    // CRC32C hardware intrinsics
    bool HasAVX2()  noexcept;    // future: wide memcpy, parallel checksum

    // ---- ARM capability queries ----
    // On non-ARM builds these always return false.
    bool HasARMCRC() noexcept;   // future: ARM CRC32 extension

} // namespace cabe::util::cpu

#endif // CABE_CPU_FEATURES_H
