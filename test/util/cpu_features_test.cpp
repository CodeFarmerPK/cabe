#include "util/cpu_features.h"

#include <gtest/gtest.h>

TEST(CpuFeatures, ArchSmoke) {
    using cabe::util::cpu::Arch;
    const auto a = cabe::util::cpu::GetArch();
    EXPECT_TRUE(a == Arch::X86_64 || a == Arch::AArch64 || a == Arch::Unknown);
}

TEST(CpuFeatures, QueriesAreStable) {
    EXPECT_EQ(cabe::util::cpu::HasSSE42(), cabe::util::cpu::HasSSE42());
    EXPECT_EQ(cabe::util::cpu::HasAVX2(), cabe::util::cpu::HasAVX2());
    EXPECT_EQ(cabe::util::cpu::HasARMCRC(), cabe::util::cpu::HasARMCRC());
}
