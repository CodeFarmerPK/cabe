#include "util/hash.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <span>
#include <string_view>

TEST(Hash, FrozenVectors) {
    // 冻结基线（D6）：变更等同 v2.0 破坏。
    EXPECT_EQ(cabe::util::Hash(std::string_view{""}), 0x2D06800538D394C2ull);
    EXPECT_EQ(cabe::util::Hash(std::string_view{"hello"}), 0x9555E8555C62DCFDull);
}

TEST(Hash, DataViewAndStringViewAgree) {
    const std::string_view s = "consistency-check";
    const auto b = std::as_bytes(std::span{s.data(), s.size()});
    EXPECT_EQ(cabe::util::Hash(s), cabe::util::Hash(b));
}

TEST(Hash, RouteInRange) {
    // 双检：① cast 为 size_t 再 EXPECT_LT，避免 uint8_t 在 n=256 档值域 [0,255] 恒真；
    //       ② 直接拿 Hash%n 作为 oracle 验值，捕 % (n+1) / off-by-one 之类的实现回归
    //          —— 仅 EXPECT_LT 在 n=256 + 错误实现下会因 uint8_t 截断静默通过。
    const auto h = cabe::util::Hash(std::string_view{"some-key"});
    for (std::size_t n : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}, std::size_t{256}}) {
        const auto d = static_cast<std::size_t>(cabe::util::RouteToDevice("some-key", n));
        EXPECT_LT(d, n) << "n=" << n;
        EXPECT_EQ(d, h % n) << "n=" << n << " (公式回归)";
    }
}

TEST(Hash, RouteDistributionUniform) {
    constexpr int kN = 8;
    constexpr int kK = 100000;
    std::array<long, kN> bucket{};
    std::mt19937_64 rng(12345);
    for (int i = 0; i < kK; ++i) {
        char key[32];
        const int len = std::snprintf(key, sizeof(key), "k%llu", static_cast<unsigned long long>(rng()));
        bucket[cabe::util::RouteToDevice(std::string_view{key, static_cast<std::size_t>(len)}, kN)]++;
    }
    const double expected = static_cast<double>(kK) / kN;
    double chi2 = 0.0;
    for (int i = 0; i < kN; ++i) {
        const double d = static_cast<double>(bucket[i]) - expected;
        chi2 += d * d / expected;
    }
    // 自由度 7，取 0.001 显著性临界 24.32（宽松，避免偶发误报）。
    EXPECT_LT(chi2, 24.32) << "chi2=" << chi2;
}
