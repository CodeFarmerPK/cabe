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

    // 仅供测试：转发到内部两条实现，用于验证"软件 fallback 与硬件 SSE4.2 路径一致"
    // （见 doc/P0/P0M5_test_bench_design.md §7.1）。业务代码请用 CRC32（运行时自动分派）。
    namespace detail {
        uint32_t SoftwareCRC32C(DataView data) noexcept;
#if defined(__x86_64__) || defined(__i386__)
        uint32_t HardwareCRC32C_x86(DataView data) noexcept; // 仅当 cpu::HasSSE42() 为真时可安全调用
#endif
    } // namespace detail
}


#endif // CABE_CRC32_H