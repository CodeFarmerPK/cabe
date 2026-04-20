/*
 * Project: Cabe
 * Created Time: 2026-04-18
 * Created by: CodeFarmerPK
 *
 * CPU 能力检测 —— 统一入口，所有 SIMD / 硬件加速模块都从这里
 * 查询特性位，禁止业务代码直接调用 __get_cpuid / __builtin_cpu_*。
 */

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
