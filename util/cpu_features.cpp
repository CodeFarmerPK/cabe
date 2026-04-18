//
// Created by root on 4/18/26.
//

#include "cpu_features.h"

#if defined(__x86_64__) || defined(__i386__)
#  include <cpuid.h>
#elif defined(__aarch64__)
#  include <sys/auxv.h>
#  include <asm/hwcap.h>
#endif

namespace cabe::util::cpu {
    namespace {

        // Detected at static-init time; const afterwards → lock-free reads.
        struct Features {
            bool sse42   = false;
            bool avx2    = false;
            bool arm_crc = false;

            Features() noexcept { Detect(); }

            void Detect() noexcept {
#if defined(__x86_64__) || defined(__i386__)
                unsigned eax, ebx, ecx, edx;
                // CPUID function 1 → ECX bit 20 = SSE4.2
                if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
                    sse42 = (ecx & (1u << 20)) != 0;
                }
                // CPUID function 7, subleaf 0 → EBX bit 5 = AVX2
                if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
                    avx2 = (ebx & (1u << 5)) != 0;
                }
#elif defined(__aarch64__)
                unsigned long hwcap = getauxval(AT_HWCAP);
                arm_crc = (hwcap & HWCAP_CRC32) != 0;
#endif
            }
        };

        const Features g_features{};

    } // namespace

    Arch GetArch() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        return Arch::X86_64;
#elif defined(__aarch64__)
        return Arch::AArch64;
#else
        return Arch::Unknown;
#endif
    }

    bool HasSSE42()  noexcept { return g_features.sse42;   }
    bool HasAVX2()   noexcept { return g_features.avx2;    }
    bool HasARMCRC() noexcept { return g_features.arm_crc; }

} // namespace cabe::util::cpu