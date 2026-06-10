/*
* Project: Cabe
 * Created Time: 2025-05-16 14:51
 * Created by: CodeFarmerPK
 */

#ifndef CABE_CRC32_H
#define CABE_CRC32_H

#include "common/structs.h"

namespace cabe::util {
    uint32_t CRC32(DataView data);

    // P5M4：流式增量 CRC32C（软件路径，复用同一张表）。供快照分块累积 data_crc——
    //   caller 自管首末：crc = 0xFFFFFFFF 起，逐块 crc = CRC32CStreamUpdate(crc, chunk)，末取 ~crc。
    //   与 CRC32 一致：~CRC32CStreamUpdate(0xFFFFFFFF, whole) == CRC32(whole)。
    uint32_t CRC32CStreamUpdate(uint32_t crc, DataView data) noexcept;

    // 仅供测试：转发到内部两条实现，用于验证"软件 fallback 与硬件 SSE4.2 路径一致"
    // （见 doc/P0/P0M5_test_bench_design.md §7.1）。业务代码请用 CRC32（运行时自动分派）。
    namespace detail {
        uint32_t SoftwareCRC32C(DataView data) noexcept;
#if defined(__x86_64__) || defined(__i386__)
        // [[gnu::target("sse4.2")]]：与内层实现一致；LTO/inline 把 intrinsic 暴露到 caller TU
        // 时仍能保有 sse4.2 codegen 上下文（P0M7 评审 #9 闭环）。
        [[gnu::target("sse4.2")]]
        uint32_t HardwareCRC32C_x86(DataView data) noexcept; // 仅当 cpu::HasSSE42() 为真时可安全调用
#endif
    } // namespace detail
}


#endif // CABE_CRC32_H