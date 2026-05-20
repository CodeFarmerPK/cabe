/*
 * Project: Cabe
 * Created Time: 2026-04-18
 * Created by: CodeFarmerPK
 *
 * CPU 能力检测实现 —— 唯一一处直接调 CPUID / HWCAP 的地方。
 * 业务模块通过 cabe::util::cpu::HasXxx() 间接查询，将来扩展
 * 新平台只改这一个文件。
 */
#include "cpu_features.h"

#if defined(__x86_64__) || defined(__i386__)
#  include <cpuid.h>
#elif defined(__aarch64__)
#  include <sys/auxv.h>
#  include <asm/hwcap.h>
#endif

namespace cabe::util::cpu {
    namespace {

        // Detect 一次后 const，读路径无锁。
        // 包装成函数内 static 的 Construct-On-First-Use，避免与
        // 其他 TU 的全局初始化产生顺序依赖（SIOF）。
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

        const Features& GetFeatures() noexcept {
            // C++11 保证线程安全 + 首次调用时初始化，
            // 调用者不用关心初始化发生在何时
            static const Features kFeatures{};
            return kFeatures;
        }

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

    bool HasSSE42()  noexcept { return GetFeatures().sse42;   }
    bool HasAVX2()   noexcept { return GetFeatures().avx2;    }
    bool HasARMCRC() noexcept { return GetFeatures().arm_crc; }
} // namespace cabe::util::cpu