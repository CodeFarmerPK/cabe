//
// Created by root on 2025-07-15.
//

#ifndef CABE_UTIL_H
#define CABE_UTIL_H

#include <chrono>
namespace cabe::util {
    static uint64_t GetTimeStamp() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();
    }
} // namespace cabe::util
#endif // CABE_UTIL_H
