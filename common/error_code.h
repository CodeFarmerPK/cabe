/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:37
 * Created by: CodeFarmerPK
 */
#ifndef CABE_ERROR_CODE_H
#define CABE_ERROR_CODE_H

#include <cstdint>

namespace cabe::err {
    inline constexpr int kSuccess = 0;

    // ---- 段基址与容量（每段 1000 号；错误码为负，段内向更负方向编号）----
    inline constexpr int kSegmentSize     = 1000;
    inline constexpr int kMemoryBase      = -100000;
    inline constexpr int kIoBase          = -101000;
    inline constexpr int kIndexBase       = -102000;
    inline constexpr int kWalBase         = -103000;
    inline constexpr int kEngineBase      = -104000;
    inline constexpr int kWalRecoveryBase = -105000;

    // ---- 段位不重叠（编译期）：相邻段恰好相距一个段容量，无缝且不交叠 ----
    static_assert(kMemoryBase - kSegmentSize == kIoBase);
    static_assert(kIoBase - kSegmentSize == kIndexBase);
    static_assert(kIndexBase - kSegmentSize == kWalBase);
    static_assert(kWalBase - kSegmentSize == kEngineBase);
    static_assert(kEngineBase - kSegmentSize == kWalRecoveryBase);

    // 段内编号：基址段的第 n 个码（n ∈ [0, kSegmentSize)）
    constexpr int InSeg(int base, int n) noexcept { return base - n; }

    // ---- memory 段（保留现有取值）----
    inline constexpr int kMemNullPointer = InSeg(kMemoryBase, 0); // -100000
    inline constexpr int kMemEmptyKey    = InSeg(kMemoryBase, 1); // -100001
    inline constexpr int kMemEmptyValue  = InSeg(kMemoryBase, 2); // -100002
    inline constexpr int kMemInsertFail  = InSeg(kMemoryBase, 3); // -100003

    // 每个码不得越段（编译期）
    static_assert(kMemInsertFail > kMemoryBase - kSegmentSize);

    // io / index / wal / engine / wal_recovery 段的具体码随各模块产生时补入。
} // namespace cabe::err
#endif // CABE_ERROR_CODE_H