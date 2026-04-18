/*
* cpu_features 单元测试 —— 验证 CPU 能力抽象层
 */
#include <gtest/gtest.h>
#include "util/cpu_features.h"

namespace cpu = cabe::util::cpu;

TEST(CpuFeaturesTest, ArchIsKnown) {
    // Cabe 当前支持的构建目标都应该能识别
    EXPECT_NE(cpu::Arch::Unknown, cpu::GetArch());
}

TEST(CpuFeaturesTest, X86BaselineHasSSE42) {
    // Fedora 43 x86_64 baseline = x86-64-v2 → SSE4.2 必有
    if (cpu::GetArch() != cpu::Arch::X86_64) {
        GTEST_SKIP() << "Not x86_64, skipping SSE4.2 check";
    }
    EXPECT_TRUE(cpu::HasSSE42())
        << "SSE4.2 should be present on any x86-64-v2 baseline";
}

TEST(CpuFeaturesTest, ARMFlagsFalseOnX86) {
    // 非 ARM 平台上 ARM 相关 feature 应全为 false（防交叉污染）
    if (cpu::GetArch() == cpu::Arch::X86_64) {
        EXPECT_FALSE(cpu::HasARMCRC());
    }
}

TEST(CpuFeaturesTest, QueriesAreStable) {
    // 多次查询结果稳定（静态初始化完成后 const）
    EXPECT_EQ(cpu::HasSSE42(), cpu::HasSSE42());
    EXPECT_EQ(cpu::HasAVX2(),  cpu::HasAVX2());
    EXPECT_EQ(cpu::GetArch(),  cpu::GetArch());
}