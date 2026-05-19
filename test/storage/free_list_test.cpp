/*
 * Project: Cabe
 * Created Time: 2026-04-03
 * Created by: CodeFarmerPK
 *
 * FreeList 单元测试(P4.5,2026-05-15)
 *
 *   M1 — SetMaxBlockCount 契约 / 严格升序 Allocate / Release 进 active /
 *        Stats / SetTrim / SetTuning(FreeListUninitTest + FreeListTest)
 *   M2 — 切换触发条件契约(FreeListSwitchTest);改造为 poll-based,
 *        与 M2 同步 / M3 异步实现均兼容(只断言切换最终发生 + 行为
 *        不变量,不断言同步专属的"立即 SwitchCount++"或"切换中
 *        不可观察")
 *   M3 — 异步 sort worker(FreeListM3Test + FreeListM3):后台线程生命
 *        周期、异步完成、Switching 可观察、析构、容量守恒
 *
 *   poll helper(DrainUntilRunning / PollUntilSwitch)定义在 M1 测试后、
 *   M2/M3 之前,两段共用。
 */

#include <gtest/gtest.h>
#include "storage/free_list.h"

#include <set>
#include <thread>
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
    EXPECT_EQ(SUCCESS, freeList_.SetMaxBlockCount(50));
    EXPECT_EQ(50u, freeList_.MaxBlockCount());
}

TEST_F(FreeListUninitTest, SetMaxBlockCountRejectsDifferentN) {
    ASSERT_EQ(SUCCESS, freeList_.SetMaxBlockCount(50));
    EXPECT_EQ(CABE_INVALID_DATA_SIZE, freeList_.SetMaxBlockCount(60));
    EXPECT_EQ(50u, freeList_.MaxBlockCount());
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
    for (uint64_t i = 0; i < kCapacity; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b)) << "iter=" << i;
        EXPECT_EQ(i, b) << "iter=" << i;
    }
}

TEST_F(FreeListTest, AllocateExhaustionReturnsNoSpace) {
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
    EXPECT_EQ(kCapacity - 1, freeList_.AllocatableCount());
    EXPECT_EQ(1u, freeList_.PendingRecycleCount());
    EXPECT_EQ(kCapacity, freeList_.FreeCount());
}

TEST_F(FreeListTest, ReleaseDoesNotImmediatelyReuse) {
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
    EXPECT_EQ(kCapacity - 3, freeList_.AllocatableCount());
    EXPECT_EQ(kCapacity, freeList_.FreeCount());
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
    ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
    ASSERT_EQ(SUCCESS, freeList_.Release(b));

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
    EXPECT_EQ(0u, freeList_.SwitchCount());

    BlockId b;
    for (int i = 0; i < 50; ++i) {
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        ASSERT_EQ(SUCCESS, freeList_.Release(b));
    }
    // 默认 min_recycle_threshold=1024,规模 50 < 1024,切换不触发
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
    freeList_.SetTuning(0.85, 0.95, 2.0, 512);
    BlockId b;
    for (uint64_t i = 0; i < 10; ++i) {
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        EXPECT_EQ(i, b);
    }
}

TEST_F(FreeListTest, SetTuningSilentlyIgnoresInvalidValues) {
    freeList_.SetTuning(-1.0, 0.0, -2.0, 0);
    BlockId b;
    EXPECT_EQ(SUCCESS, freeList_.Allocate(&b));
}

// ============================================================
// RebuildFromActive(M1/M2/M3 占位,M4 完整实现)
// ============================================================

TEST_F(FreeListTest, RebuildFromActiveM1StubReturnsNotImplemented) {
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
    constexpr int kHalf = 50;
    std::vector<BlockId> allocated;
    allocated.reserve(kHalf);

    for (int i = 0; i < kHalf; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        allocated.push_back(b);
    }
    for (BlockId b : allocated) {
        ASSERT_EQ(SUCCESS, freeList_.Release(b));
    }
    EXPECT_EQ(kHalf, static_cast<int>(freeList_.PendingRecycleCount()));
    EXPECT_EQ(kCapacity - kHalf, freeList_.AllocatableCount());

    for (uint64_t expect = kHalf; expect < kCapacity; ++expect) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        EXPECT_EQ(expect, b);
    }
    EXPECT_EQ(0u, freeList_.AllocatableCount());
    EXPECT_EQ(kHalf, static_cast<int>(freeList_.PendingRecycleCount()));
}

// ============================================================
// poll helper(M2/M3 共用)— 实现无关地驱动 / 等待切换完成
//
//   M2 同步 sort:StartSwitch 内已 sort 完;poll 第一次 Allocate 即
//                 CompleteSwitch,立即返回。
//   M3 异步 sort:StartSwitch 仅提交任务,后台线程 sort;poll 反复
//                 Allocate 驱动入口 CompleteSwitch,freeList 空时 yield
//                 让后台推进。
//   小数据 sort 微秒级,实际几个 spin 内完成;1000 万自旋上限防 flaky。
// ============================================================

bool DrainUntilRunning(FreeList& fl, std::vector<BlockId>* sink) {
    for (int spin = 0; spin < 10'000'000; ++spin) {
        if (!fl.IsSortInProgress()) {
            return true;
        }
        BlockId b;
        if (fl.Allocate(&b) == SUCCESS) {
            if (sink != nullptr) {
                sink->push_back(b);
            }
        } else {
            std::this_thread::yield();
        }
    }
    return false;
}

bool PollUntilSwitch(FreeList& fl, uint64_t target) {
    for (int spin = 0; spin < 10'000'000; ++spin) {
        if (fl.SwitchCount() >= target) {
            return true;
        }
        BlockId b;
        if (fl.Allocate(&b) != SUCCESS) {
            std::this_thread::yield();
        }
    }
    return false;
}

// ============================================================
// P4.5 M2 — 切换触发条件契约(poll-based,实现无关)
//
//   fixture 用 SetTuning 设小阈值(min_recycle_threshold=4,
//   switch_ratio=0.75 即 freeList 剩 ≤ 25% 触发)便于小规模触发切换。
//   只断言"切换最终发生 + 行为不变量",不断言同步专属行为。
//   容量守恒由 M3 CapacityConservedAsyncSwitch 覆盖,此处不重复。
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

// 1. freeList 阈值触发切换(free ≤ initial × 0.25 → 切换最终发生)
TEST_F(FreeListSwitchTest, FreeListThresholdTriggersSwitch) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 10; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        taken.push_back(b);
    }
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(SUCCESS, freeList_.Release(taken[i]));  // active=5 ≥ 4
    }
    EXPECT_EQ(0u, freeList_.SwitchCount());

    // Allocate 把 freeList 拉到 ≤ 25 → 触发 StartSwitch
    for (int i = 0; i < 66; ++i) {
        BlockId b;
        if (freeList_.Allocate(&b) != SUCCESS) break;
    }
    // poll 驱动 CompleteSwitch(M2 同步即返,M3 异步需 poll)
    ASSERT_TRUE(PollUntilSwitch(freeList_, 1));
    EXPECT_GE(freeList_.SwitchCount(), 1u);
}

// 2. 对称水位触发切换(free>25 时仅 symmetric/pending 条件成立)
TEST_F(FreeListSwitchTest, SymmetricWatermarkTriggersSwitch) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 70; ++i) {  // freeList=30 > 25,free_threshold 不成立
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        taken.push_back(b);
    }
    EXPECT_EQ(0u, freeList_.SwitchCount());
    for (int i = 0; i < 46; ++i) {  // active ≥ 30×1.5=45 → switch_pending_
        ASSERT_EQ(SUCCESS, freeList_.Release(taken[i]));
    }
    BlockId b;
    (void) freeList_.Allocate(&b);  // 触发 StartSwitch(pending / symmetric)
    ASSERT_TRUE(PollUntilSwitch(freeList_, 1));
    EXPECT_GE(freeList_.SwitchCount(), 1u);
}

// 3. min_recycle_threshold 阻止启动初期切换(active < 4,永不切换)
TEST_F(FreeListSwitchTest, MinRecycleThresholdBlocksSwitch) {
    for (int i = 0; i < 90; ++i) {  // freeList=10,纯 Allocate active=0
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
    }
    EXPECT_EQ(0u, freeList_.SwitchCount());  // 前置不满足,绝不切换
}

// 4. 切换后分配仍正确:BlockId 合法 + 无重复(跨切换会跳号,严格升序
//    仅在单批 sorted freeList 内成立,设计如此 §2.2;此处验证不变量)
TEST_F(FreeListSwitchTest, AllocateValidAfterSwitch) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 50; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, freeList_.Allocate(&b));
        taken.push_back(b);
    }
    for (int i = 49; i >= 0; --i) {  // 倒序 Release → active 累积
        ASSERT_EQ(SUCCESS, freeList_.Release(taken[i]));
    }
    for (int i = 0; i < 30; ++i) {   // 触发 StartSwitch
        BlockId b;
        if (freeList_.Allocate(&b) != SUCCESS) break;
    }
    ASSERT_TRUE(PollUntilSwitch(freeList_, 1));
    ASSERT_GE(freeList_.SwitchCount(), 1u);

    std::set<BlockId> seen;
    int got = 0;
    for (int i = 0; i < 20; ++i) {
        BlockId b;
        if (freeList_.Allocate(&b) != SUCCESS) break;
        EXPECT_LT(b, kCapacity);
        EXPECT_TRUE(seen.insert(b).second) << "dup blockId " << b;
        ++got;
    }
    EXPECT_GT(got, 0);  // 切换后仍能继续分配
}

// ============================================================
// P4.5 M3 — 异步 sort worker
//
//   sort 由后台线程执行,业务线程 StartSwitch 仅提交任务 + notify;
//   CompleteSwitch 由下次 Allocate 入口检测 sort_done_ 触发。
//   poll helper 见上方 M2 块前定义,M2/M3 共用。
// ============================================================

class FreeListM3Test : public ::testing::Test {
protected:
    static constexpr uint64_t kCapacity = 100;
    void SetUp() override {
        ASSERT_EQ(SUCCESS, fl_.SetMaxBlockCount(kCapacity));
        fl_.SetTuning(0.75, 0.90, 1.5, 4);
    }
    FreeList fl_;
};

// 1. 后台 worker 生命周期:多次构造析构(含 n=0)不 hang(析构 join)
TEST(FreeListM3, WorkerLifecycleNoHang) {
    for (int i = 0; i < 20; ++i) {
        FreeList fl;
        ASSERT_EQ(SUCCESS, fl.SetMaxBlockCount(i % 2 == 0 ? 0u : 50u));
    }
    SUCCEED();
}

// 2. 异步 sort 最终完成切换
TEST_F(FreeListM3Test, AsyncSortCompletesSwitch) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 10; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, fl_.Allocate(&b));
        taken.push_back(b);
    }
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(SUCCESS, fl_.Release(taken[i]));
    }
    EXPECT_EQ(0u, fl_.SwitchCount());

    for (int i = 0; i < 66; ++i) {
        BlockId b;
        if (fl_.Allocate(&b) != SUCCESS) break;
    }
    ASSERT_TRUE(PollUntilSwitch(fl_, 1));
    EXPECT_GE(fl_.SwitchCount(), 1u);
    ASSERT_TRUE(DrainUntilRunning(fl_, nullptr));
    EXPECT_FALSE(fl_.IsSortInProgress());
}

// 3. 切换中状态可观察(M3 异步:StartSwitch 后 state=Switching 持续到
//    下次 Allocate 入口 CompleteSwitch;M2 同步时不可观察)
TEST_F(FreeListM3Test, SwitchInProgressObservable) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 10; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, fl_.Allocate(&b));
        taken.push_back(b);
    }
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(SUCCESS, fl_.Release(taken[i]));
    }
    bool observed = false;
    for (int i = 0; i < 66; ++i) {
        BlockId b;
        if (fl_.Allocate(&b) != SUCCESS) break;
        if (fl_.IsSortInProgress()) {  // 业务线程串行设 state,无 race
            observed = true;
            break;
        }
    }
    EXPECT_TRUE(observed);
    ASSERT_TRUE(DrainUntilRunning(fl_, nullptr));
    EXPECT_FALSE(fl_.IsSortInProgress());
    EXPECT_GE(fl_.SwitchCount(), 1u);
}

// 4. StartSwitch 异步证据:提交后 SwitchCount 未增、state=Switching
//    (区别于 M2 同步:StartSwitch 后立即 SwitchCount++)
TEST_F(FreeListM3Test, StartSwitchAsyncNotInlineComplete) {
    std::vector<BlockId> taken;
    for (int i = 0; i < 10; ++i) {
        BlockId b;
        ASSERT_EQ(SUCCESS, fl_.Allocate(&b));
        taken.push_back(b);
    }
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(SUCCESS, fl_.Release(taken[i]));
    }
    const uint64_t before = fl_.SwitchCount();
    for (int i = 0; i < 66; ++i) {
        BlockId b;
        if (fl_.Allocate(&b) != SUCCESS) break;
        if (fl_.IsSortInProgress()) break;  // 命中 StartSwitch,停止再调 Allocate
    }
    EXPECT_EQ(before, fl_.SwitchCount());
    EXPECT_TRUE(fl_.IsSortInProgress());
    ASSERT_TRUE(DrainUntilRunning(fl_, nullptr));
    EXPECT_GT(fl_.SwitchCount(), before);
}

// 5. 切换中析构不 hang(StartSwitch 后立即销毁,worker join 正常退出)
TEST(FreeListM3, DestructDuringSwitchNoHang) {
    {
        FreeList fl;
        ASSERT_EQ(SUCCESS, fl.SetMaxBlockCount(100));
        fl.SetTuning(0.75, 0.90, 1.5, 4);
        std::vector<BlockId> taken;
        for (int i = 0; i < 10; ++i) {
            BlockId b;
            ASSERT_EQ(SUCCESS, fl.Allocate(&b));
            taken.push_back(b);
        }
        for (int i = 0; i < 5; ++i) {
            ASSERT_EQ(SUCCESS, fl.Release(taken[i]));
        }
        for (int i = 0; i < 66; ++i) {
            BlockId b;
            if (fl.Allocate(&b) != SUCCESS) break;
            if (fl.IsSortInProgress()) break;  // 切换中,立即结束作用域析构
        }
        // fl 析构:即使后台 sort 进行中,stop_ + notify + join 正常退出
    }
    SUCCEED();
}

// 6. 异步切换下容量守恒(仅 Running 时断言;最终 poll 到 Running 必守恒)
TEST_F(FreeListM3Test, CapacityConservedAsyncSwitch) {
    std::vector<BlockId> live;
    for (int round = 0; round < 400; ++round) {
        BlockId b;
        if (fl_.Allocate(&b) == SUCCESS) {
            live.push_back(b);
        }
        if (live.size() >= 5) {
            ASSERT_EQ(SUCCESS, fl_.Release(live.back()));
            live.pop_back();
        }
        // M3 异步:切换中 sort_task_/sort_result_ 被后台持有,不计入
        // FreeCount;仅 Running(切换完成,缓冲空)时守恒成立。
        if (!fl_.IsSortInProgress()) {
            EXPECT_EQ(fl_.FreeCount() + live.size(), kCapacity)
                << "leak at round " << round;
        }
    }
    ASSERT_TRUE(DrainUntilRunning(fl_, &live));
    EXPECT_EQ(fl_.FreeCount() + live.size(), kCapacity);
    EXPECT_GE(fl_.SwitchCount(), 1u);
}

}  // namespace
