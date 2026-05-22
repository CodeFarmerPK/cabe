#include "util/util.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>

TEST(Util, MonotonicNonDecreasing) {
    const auto a = cabe::util::GetMonotonicTimeNs();
    const auto b = cabe::util::GetMonotonicTimeNs();
    EXPECT_GE(b, a);
}

TEST(Util, MonotonicAdvancesAfterSleep) {
    const auto a = cabe::util::GetMonotonicTimeNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto b = cabe::util::GetMonotonicTimeNs();
    EXPECT_GT(b, a);
}

TEST(Util, WallTimeNonZero) {
    EXPECT_GT(cabe::util::GetWallTimeNs(), std::uint64_t{0});
}
