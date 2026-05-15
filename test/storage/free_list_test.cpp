/*
 * Project: Cabe
 * Created Time: 2026-04-03
 * Created by: CodeFarmerPK
 *
 * FreeList 单元测试(P4.5 M1 重写,2026-05-15)
 *
 *   覆盖范围(M1 接口语义):
 *     - SetMaxBlockCount 全量装载 + 幂等契约
 *     - 严格升序 Allocate(D-2)
 *     - Release / ReleaseBatch 进 active recycle(不立即可分配)
 *     - Stats getter(FreeCount 全局 / AllocatableCount 即时 / PendingRecycleCount 等)
 *     - SetTrimContext / SetTuning 上下文存储
 *
 *   不覆盖(留后续 milestone):
 *     - 切换触发(M2)
 *     - 后台 sort 异步(M3)
 *     - TRIM ioctl / 写保护 / 完整 RebuildFromActive(M4)
 *     - Engine 集成测试(M5)
 */

#include <gtest/gtest.h>
#include "storage/free_list.h"

#include <set>
#include <vector>

namespace {

class FreeListTest : public ::testing::Test {
protected:
    static constexpr uint64_t kCapacity = 100;

    void SetUp() override {
        ASSERT_EQ(SUCCESS, freeList_.SetMaxBlockCount(kCapacity));
    }

    FreeList freeList_;
};

// 单独的未初始化测试套件 — fixture 不调 SetMaxBlockCount
class FreeListUninitTest : public ::testing::Test {
protected:
    FreeList freeList_;
};

// ============================================================
// SetMaxBlockCount 契约
// ============================================================

TEST_F(FreeListUninitTest, SetMaxBlockCountZeroIsLegal) {
    // 容量 0 是单测 fixture 合法边界,Allocate 立即 NO_SPACE
    ASSERT_EQ(SUCCESS, freeList_.SetMaxBlockCount(0));
    BlockId b;
    EXPECT_EQ(DEVICE_NO_SPACE, freeList_.Allocate(&b));
    EXPECT_EQ(0u, freeList_.AllocatableCount());
}

TEST_F(FreeListUninitTest, SetMaxBlockCountLoadsAllBlockIds) {
    ASSERT_EQ(SUCCESS, freeList_.SetMaxBlockCount(50));
    EXPECT_EQ(50u, freeList_.AllocatableCount());
    EXPECT_EQ(50u, freeList_.FreeCount());
    EXPECT_EQ(50u, freeList_.MaxBlockCount());
}

TEST_F(FreeListUninitTest, SetMaxBlockCountIdempotentSameN) {
    ASSERT_EQ(SUCCESS, freeList_.SetMaxBlockCount(50));
    EXPECT_EQ(SUCCESS, freeList_.SetMaxBlockCount(50));  // 同 n 幂等
    EXPECT_EQ(50u, freeList_.MaxBlockCount());
}

TEST_F(FreeListUninitTest, SetMaxBlockCountRejectsDifferentN) {
    ASSERT_EQ(SUCCESS, freeList_.SetMaxBlockCount(50));
    EXPECT_EQ(CABE_INVALID_DATA_SIZE, freeList_.SetMaxBlockCount(60));
    EXPECT_EQ(50u, freeList_.MaxBlockCount());  // 首次值保留
}

// ============================================================
// Allocate 基本操作 + 严格升序
// ============================================================

TEST_F(FreeListTest, AllocateNullPtrReturnsError) {
    EXPECT_EQ(MEMORY_NULL_POINTER_EXCEPTION, freeList_.Allocate(nullptr));
}

TEST_F(FreeListUninitTest, AllocateBeforeSetMaxRejected) {
    BlockId b;
    EXPECT_EQ(POOL_NOT_INITIALIZED, freeList_.Allocate(&b));
}

TEST_F(FreeListTest, AllocateStrictlyAscendingFromZero) {
    // P4.5 核心契约:严格升序,从 0 开始连续分配
    for (uint64_t i = 0; i < kCapacity; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b)) << "iter=" << i;
        EXPECT_EQ(i, b) << "iter=" << i;
    }
}

TEST_F(FreeListTest, AllocateExhaustionReturnsNoSpace) {
    // 消耗光所有 BlockId
    for (uint64_t i = 0; i < kCapacity; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
    }
    BlockId b;
    EXPECT_EQ(DEVICE_NO_SPACE, freeList_.Allocate(&b));
    EXPECT_EQ(0u, freeList_.AllocatableCount());
}

TEST_F(FreeListTest, AllocateDecreasesAllocatableCount) {
    EXPECT_EQ(kCapacity, freeList_.AllocatableCount());
    BlockId b;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
    EXPECT_EQ(kCapacity - 1, freeList_.AllocatableCount());
}

// ============================================================
// Release 进 active(不立即可分配)— P4.5 核心行为变更
// ============================================================

TEST_F(FreeListTest, ReleaseGoesToActiveNotAllocatable) {
    BlockId b;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
    EXPECT_EQ(0u, b);

    ASSERT_EQ(SUCCESS, freeList_.Release(b));
    // P4.5 关键:Release 后不增加 AllocatableCount,只增加 PendingRecycleCount
    EXPECT_EQ(kCapacity - 1, freeList_.AllocatableCount());   // 仅 freeList
    EXPECT_EQ(1u, freeList_.PendingRecycleCount());            // active 增 1
    EXPECT_EQ(kCapacity, freeList_.FreeCount());               // 全局可用回到 kCapacity
}

TEST_F(FreeListTest, ReleaseDoesNotImmediatelyReuse) {
    // P4.5 关键:Release 后 Allocate 拿到下一个未分配的最小 BlockId,
    //         而不是刚 Release 的那个(M1 阶段未引入切换,这些将永远在 active)
    BlockId b0, b1;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b0));   // 拿到 0
    ASSERT_EQ(SUCCESS, freeList_.Release(b0));     // 0 进 active

    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b1));   // 拿到 1(不是 0)
    EXPECT_EQ(1u, b1);
}

TEST_F(FreeListUninitTest, ReleaseBeforeSetMaxRejected) {
    EXPECT_EQ(POOL_NOT_INITIALIZED, freeList_.Release(0));
}

// ============================================================
// ReleaseBatch 原子性
// ============================================================

TEST_F(FreeListTest, ReleaseBatchPushesAllToActive) {
    BlockId b0, b1, b2;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b0));
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b1));
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b2));

    const std::vector<BlockId> batch = {b0, b1, b2};
    ASSERT_EQ(SUCCESS, freeList_.ReleaseBatch(batch));

    EXPECT_EQ(3u, freeList_.PendingRecycleCount());
    EXPECT_EQ(kCapacity - 3, freeList_.AllocatableCount());   // freeList 不增
    EXPECT_EQ(kCapacity, freeList_.FreeCount());               // 全局回到 kCapacity
}

TEST_F(FreeListTest, ReleaseBatchEmpty) {
    const std::vector<BlockId> empty;
    ASSERT_EQ(SUCCESS, freeList_.ReleaseBatch(empty));
    EXPECT_EQ(0u, freeList_.PendingRecycleCount());
}

TEST_F(FreeListUninitTest, ReleaseBatchBeforeSetMaxRejected) {
    const std::vector<BlockId> batch = {0, 1};
    EXPECT_EQ(POOL_NOT_INITIALIZED, freeList_.ReleaseBatch(batch));
}

// ============================================================
// Stats getter 语义
// ============================================================

TEST_F(FreeListTest, FreeCountIsGlobalSum) {
    BlockId b;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));    // freeList -1
    ASSERT_EQ(SUCCESS, freeList_.Release(b));      // active +1

    EXPECT_EQ(freeList_.AllocatableCount() + freeList_.PendingRecycleCount(),
              freeList_.FreeCount());
    EXPECT_EQ(kCapacity, freeList_.FreeCount());
}

TEST_F(FreeListUninitTest, StatsInitiallyZeroBeforeSetMax) {
    EXPECT_EQ(0u, freeList_.FreeCount());
    EXPECT_EQ(0u, freeList_.AllocatableCount());
    EXPECT_EQ(0u, freeList_.PendingRecycleCount());
    EXPECT_EQ(0u, freeList_.SwitchCount());
    EXPECT_EQ(0u, freeList_.MaxBlockCount());
    EXPECT_FALSE(freeList_.TrimSupported());
    EXPECT_FALSE(freeList_.IsSortInProgress());
}

TEST_F(FreeListTest, SwitchCountInitiallyZero) {
    // M1 阶段未实现切换,SwitchCount 应保持 0
    EXPECT_EQ(0u, freeList_.SwitchCount());

    BlockId b;
    for (int i = 0; i < 50; ++i) {
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        ASSERT_EQ(SUCCESS, freeList_.Release(b));
    }
    // 即便消耗 / 释放,M1 不触发切换,SwitchCount 仍为 0
    EXPECT_EQ(0u, freeList_.SwitchCount());
}

TEST_F(FreeListTest, IsSortInProgressInitiallyFalse) {
    EXPECT_FALSE(freeList_.IsSortInProgress());
}

TEST_F(FreeListTest, MaxBlockCountMatchesConfigured) {
    EXPECT_EQ(kCapacity, freeList_.MaxBlockCount());
}

// ============================================================
// SetTrimContext / SetTuning 上下文存储
// ============================================================

TEST_F(FreeListTest, SetTrimContextStores) {
    EXPECT_FALSE(freeList_.TrimSupported());
    freeList_.SetTrimContext(42, true);
    EXPECT_TRUE(freeList_.TrimSupported());

    freeList_.SetTrimContext(-1, false);
    EXPECT_FALSE(freeList_.TrimSupported());
}

TEST_F(FreeListTest, SetTuningAcceptsValidValues) {
    // 调用后行为不应改变 M1 的 Allocate / Release 路径(切换 / 写保护未接入)
    freeList_.SetTuning(0.85, 0.95, 2.0, 512);

    BlockId b;
    for (uint64_t i = 0; i < 10; ++i) {
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        EXPECT_EQ(i, b);  // 升序行为不受 tuning 影响(M1 还没接入)
    }
}

TEST_F(FreeListTest, SetTuningSilentlyIgnoresInvalidValues) {
    // 0 / 负值 / >= 1 等非法值静默忽略,保留已有默认
    freeList_.SetTuning(-1.0, 0.0, -2.0, 0);
    // 行为仍然正常
    BlockId b;
    EXPECT_EQ(SUCCESS, freeList_.Allocate(&b));
}

// ============================================================
// RebuildFromActive (M1 占位行为)
// ============================================================

TEST_F(FreeListTest, RebuildFromActiveM1StubReturnsNotImplemented) {
    // M1 stub 返回 CABE_INVALID_DATA_SIZE(M4 真实现)
    std::vector<BlockId> active = {1, 2, 3};
    EXPECT_EQ(CABE_INVALID_DATA_SIZE,
              freeList_.RebuildFromActive(std::span<const BlockId>(active)));
}

// ============================================================
// 压力 + 行为一致性
// ============================================================

TEST_F(FreeListTest, ExhaustAllUniqueAndAscending) {
    std::set<BlockId> seen;
    BlockId last = 0;
    bool first = true;
    for (uint64_t i = 0; i < kCapacity; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        EXPECT_TRUE(seen.insert(b).second) << "duplicate at " << i;
        if (!first) {
            EXPECT_GT(b, last) << "not ascending at " << i;
        }
        last = b;
        first = false;
    }
    EXPECT_EQ(kCapacity, seen.size());
}

TEST_F(FreeListTest, ReleaseAndAllocateStressBeforeSwitch) {
    // M1 阶段:Release 后 Allocate 不复用,持续消耗 freeList
    constexpr int kHalf = 50;
    std::vector<BlockId> allocated;
    allocated.reserve(kHalf);

    for (int i = 0; i < kHalf; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        allocated.push_back(b);
    }
    // 释放一半到 active
    for (BlockId b : allocated) {
        ASSERT_EQ(SUCCESS, freeList_.Release(b));
    }
    EXPECT_EQ(kHalf, static_cast<int>(freeList_.PendingRecycleCount()));
    EXPECT_EQ(kCapacity - kHalf, freeList_.AllocatableCount());

    // 继续 Allocate,拿到的是 freeList 后半段(50..99 升序)
    for (uint64_t expect = kHalf; expect < kCapacity; ++expect) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        EXPECT_EQ(expect, b);
    }
    EXPECT_EQ(0u, freeList_.AllocatableCount());
    EXPECT_EQ(kHalf, static_cast<int>(freeList_.PendingRecycleCount()));
}

// ============================================================
// P4.5 M2 — 切换路径(StartSwitch / CompleteSwitch / 状态机)
//
//   fixture 用 SetTuning 把 min_recycle_threshold 设为 4(生产默认 1024),
//   switch_ratio=0.75(freeList 剩 ≤ 25% 触发),便于小规模单测触发切换。
//   M2 阶段 sort 同步执行,切换在 Allocate 调用栈内完成。
// ============================================================

class FreeListSwitchTest : public ::testing::Test {
protected:
    static constexpr uint64_t kCapacity = 100;
    void SetUp() override {
        ASSERT_EQ(SUCCESS, freeList_.SetMaxBlockCount(kCapacity));
        freeList_.SetTuning(0.75, 0.90, 1.5, 4);
    }
    FreeList freeList_;
};

// 1. freeList 阈值触发切换(free.size ≤ initial × 0.25)
TEST_F(FreeListSwitchTest, FreeListThresholdTriggersSwitch) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 10; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        taken.push_back(b);
    }
    // 满足 min_recycle_threshold(=4)前置
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(SUCCESS, freeList_.Release(taken[i]));
    }
    EXPECT_EQ(0u, freeList_.SwitchCount());

    // 继续 Allocate 把 freeList 拉到 ≤ 25(100 × 0.25)→ 触发切换
    for (int i = 0; i < 66; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
    }
    EXPECT_GE(freeList_.SwitchCount(), 1u);
}

// 2. 对称水位触发切换(free 保持 > 25,只让 symmetric 条件成立)
TEST_F(FreeListSwitchTest, SymmetricWatermarkTriggersSwitch) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 70; ++i) {  // freeList=30 > 25,free_threshold 不成立
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        taken.push_back(b);
    }
    EXPECT_EQ(0u, freeList_.SwitchCount());
    // Release 到 active ≥ free(30) × 1.5 = 45 → 标记 switch_pending_
    for (int i = 0; i < 46; ++i) {
        ASSERT_EQ(SUCCESS, freeList_.Release(taken[i]));
    }
    // 下一次 Allocate 触发(pending / symmetric,非 free_threshold)
    BlockId b;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
    EXPECT_GE(freeList_.SwitchCount(), 1u);
}

// 3. min_recycle_threshold 阻止启动初期切换(active < 4)
TEST_F(FreeListSwitchTest, MinRecycleThresholdBlocksSwitch) {
    // 纯 Allocate 不 Release:active=0 < min_recycle_threshold(4)
    for (int i = 0; i < 90; ++i) {  // freeList=10,远低于 25 阈值
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
    }
    EXPECT_EQ(0u, freeList_.SwitchCount());  // 前置不满足,绝不切换
}

// 4. 切换后分配仍严格升序
TEST_F(FreeListSwitchTest, AscendingAfterSwitch) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 50; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        taken.push_back(b);
    }
    // 倒序 Release 全部 50 个 → active 累积(乱序进 active)
    for (int i = 49; i >= 0; --i) {
        ASSERT_EQ(SUCCESS, freeList_.Release(taken[i]));
    }
    // Allocate 触发切换(freeList 50 → ≤ 25)
    for (int i = 0; i < 30; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
    }
    ASSERT_GE(freeList_.SwitchCount(), 1u);

    // 切换后 sort 已把回收 BlockId 降序排列,pop_back 仍取最小 → 严格升序
    BlockId prev;
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&prev));
    for (int i = 0; i < 10; ++i) {
        BlockId b;
        if (freeList_.Allocate(&b) != SUCCESS) break;
        EXPECT_GT(b, prev) << "not ascending after switch at i=" << i;
        prev = b;
    }
}

// 5. 多次切换 SwitchCount 单调不减 + 容量守恒
TEST_F(FreeListSwitchTest, MultipleSwitchesMonotonicCount) {
    std::vector<BlockId> live;
    uint64_t prevSwitch = 0;
    for (int round = 0; round < 100; ++round) {
        BlockId b;
        if (freeList_.Allocate(&b) == SUCCESS) {
            live.push_back(b);
        }
        if (live.size() >= 6) {
            ASSERT_EQ(SUCCESS, freeList_.Release(live.front()));
            live.erase(live.begin());
        }
        const uint64_t cur = freeList_.SwitchCount();
        EXPECT_GE(cur, prevSwitch) << "SwitchCount decreased at round " << round;
        prevSwitch = cur;
        EXPECT_EQ(freeList_.FreeCount() + live.size(), kCapacity)
            << "capacity leak at round " << round;
    }
    EXPECT_GE(freeList_.SwitchCount(), 1u);
}

// 6. M2 同步 sort:Switching 状态外部不可观察(Allocate 返回时已回 Running)
TEST_F(FreeListSwitchTest, SwitchStateNotObservableInM2Sync) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 80; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        taken.push_back(b);
    }
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(SUCCESS, freeList_.Release(taken[i]));
    }
    for (int i = 0; i < 60; ++i) {
        BlockId b;
        if (freeList_.Allocate(&b) != SUCCESS) break;
    }
    EXPECT_FALSE(freeList_.IsSortInProgress());
    EXPECT_GE(freeList_.SwitchCount(), 1u);
}

// 7. 跨多轮切换的容量守恒(残余不丢失,无泄漏)
TEST_F(FreeListSwitchTest, CapacityConservedAcrossSwitches) {
    std::vector<BlockId> live;
    for (int round = 0; round < 300; ++round) {
        BlockId b;
        if (freeList_.Allocate(&b) == SUCCESS) {
            live.push_back(b);
        }
        if (live.size() >= 5) {
            ASSERT_EQ(SUCCESS, freeList_.Release(live.back()));
            live.pop_back();
        }
        // 三容器全局可用 + 已分配在外的 == 总容量,任意时刻恒成立
        EXPECT_EQ(freeList_.FreeCount() + live.size(), kCapacity)
            << "capacity leak at round " << round
            << " switchCount=" << freeList_.SwitchCount();
    }
    EXPECT_GE(freeList_.SwitchCount(), 1u);
}

}  // namespace
