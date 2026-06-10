/*
* Project: Cabe
 * Created Time: 2025-07-15
 * Created by: CodeFarmerPK
 *
 * 通用小工具。目前只放了 GetTimeStamp，未来添加的任何"跨模块
 * 一句话级"的函数都可以放这里（放多了就该拆文件）。
 */

#ifndef CABE_UTIL_H
#define CABE_UTIL_H

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace cabe::util {
    // 墙钟时间戳，单位纳秒。
    // 用途：ValueMeta.timestamp 等"用户可见的时间"。
    // 特性：跨进程可比、NTP 同步后有意义；可能被 NTP 回调，不保证单调。
    inline uint64_t GetWallTimeNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
    }
    // 单调时间戳，单位纳秒。
    // 用途：性能计时（bench 里算延迟差），保证单调、不受 NTP 影响。
    // 特性：无跨进程含义，值的绝对大小不可对外暴露。
    inline uint64_t GetMonotonicTimeNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // 把 x 向上取整到 a 的倍数（a 须为正）。用于 4K / O_DIRECT 对齐等。
    constexpr std::size_t AlignUp(std::size_t x, std::size_t a) noexcept {
        return ((x + a - 1) / a) * a;
    }

    // 把"用户配置的缓冲大小"规整为可直接用于 O_DIRECT 的值：钳到 ≥ block 且向上取整到
    // block 的倍数。Wal::Open 与 Snapshot::Write 共用（P5M4，消除两处重复）。
    constexpr std::size_t RoundUpBufferSize(std::size_t size, std::size_t block) noexcept {
        return AlignUp(size < block ? block : size, block);
    }

} // namespace cabe::util

#endif // CABE_UTIL_H