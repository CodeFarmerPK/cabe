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
#include <cstdint>

namespace cabe::util {
    // 墙钟时间戳，单位纳秒。
    // 用途：KeyMeta.createdAt / modifiedAt 等"用户可见的时间"。
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

} // namespace cabe::util

#endif // CABE_UTIL_H
