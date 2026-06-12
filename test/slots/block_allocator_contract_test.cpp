#include "slots/ring/ring_block_allocator.h"

#include <gtest/gtest.h>

#include <set>
#include <vector>

template<typename T>
class BlockAllocatorContractTest : public ::testing::Test {
protected:
    T allocator_;
};

using BlockAllocatorImpls = ::testing::Types<cabe::RingBlockAllocator>;
TYPED_TEST_SUITE(BlockAllocatorContractTest, BlockAllocatorImpls);

TYPED_TEST(BlockAllocatorContractTest, InitFillsAll) {
    EXPECT_EQ(this->allocator_.Init(0, 100), cabe::err::kSuccess);
    EXPECT_EQ(this->allocator_.Available(), 100u);
    EXPECT_FALSE(this->allocator_.Empty());
}

TYPED_TEST(BlockAllocatorContractTest, AcquireReturnsBlock) {
    this->allocator_.Init(0, 5);
    cabe::BlockId bid{};
    EXPECT_EQ(this->allocator_.Acquire(&bid), cabe::err::kSuccess);
    EXPECT_EQ(bid.dev(), 0);
}

TYPED_TEST(BlockAllocatorContractTest, AcquireExhausted) {
    this->allocator_.Init(0, 3);
    cabe::BlockId bid{};
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(this->allocator_.Acquire(&bid), cabe::err::kSuccess);
    }
    EXPECT_EQ(this->allocator_.Acquire(&bid), cabe::err::kEngineNoSpace);
    EXPECT_TRUE(this->allocator_.Empty());
}

TYPED_TEST(BlockAllocatorContractTest, RecycleRestores) {
    this->allocator_.Init(0, 2);
    cabe::BlockId bid{};
    this->allocator_.Acquire(&bid);
    EXPECT_EQ(this->allocator_.Available(), 1u);

    this->allocator_.Recycle(bid);
    EXPECT_EQ(this->allocator_.Available(), 2u);
}

TYPED_TEST(BlockAllocatorContractTest, FifoOrder) {
    this->allocator_.Init(0, 5);
    for (std::uint64_t i = 0; i < 5; ++i) {
        cabe::BlockId bid{};
        EXPECT_EQ(this->allocator_.Acquire(&bid), cabe::err::kSuccess);
        EXPECT_EQ(bid.block_idx(), i);
    }
}

TYPED_TEST(BlockAllocatorContractTest, RecycledGoesToTail) {
    this->allocator_.Init(0, 3);

    cabe::BlockId first{};
    this->allocator_.Acquire(&first);
    EXPECT_EQ(first.block_idx(), 0u);

    this->allocator_.Recycle(first);

    cabe::BlockId second{};
    this->allocator_.Acquire(&second);
    EXPECT_EQ(second.block_idx(), 1u);

    cabe::BlockId third{};
    this->allocator_.Acquire(&third);
    EXPECT_EQ(third.block_idx(), 2u);

    cabe::BlockId recycled{};
    this->allocator_.Acquire(&recycled);
    EXPECT_EQ(recycled.block_idx(), 0u);
}

TYPED_TEST(BlockAllocatorContractTest, DeviceIdPreserved) {
    this->allocator_.Init(5, 3);
    cabe::BlockId bid{};
    this->allocator_.Acquire(&bid);
    EXPECT_EQ(bid.dev(), 5);
}

TYPED_TEST(BlockAllocatorContractTest, RebuildFromActive) {
    std::vector<cabe::BlockId> active = {
        cabe::BlockId::Make(0, 1),
        cabe::BlockId::Make(0, 3),
    };
    EXPECT_EQ(this->allocator_.RebuildFromActive(0, 5, active), cabe::err::kSuccess);
    EXPECT_EQ(this->allocator_.Available(), 3u);
}

TYPED_TEST(BlockAllocatorContractTest, RebuildExcludesActive) {
    std::vector<cabe::BlockId> active = {
        cabe::BlockId::Make(0, 1),
        cabe::BlockId::Make(0, 3),
    };
    this->allocator_.RebuildFromActive(0, 5, active);

    std::set<std::uint64_t> acquired;
    cabe::BlockId bid{};
    while (this->allocator_.Acquire(&bid) == cabe::err::kSuccess) {
        acquired.insert(bid.block_idx());
    }
    EXPECT_EQ(acquired, (std::set<std::uint64_t>{0, 2, 4}));
}

TYPED_TEST(BlockAllocatorContractTest, MoveConstruct) {
    this->allocator_.Init(0, 10);
    cabe::BlockId bid{};
    this->allocator_.Acquire(&bid);

    auto moved = std::move(this->allocator_);
    EXPECT_EQ(moved.Available(), 9u);
    EXPECT_TRUE(this->allocator_.Empty());
}

TYPED_TEST(BlockAllocatorContractTest, ConceptSatisfied) {
    static_assert(cabe::BlockAllocator<TypeParam>);
}

// P5M6：RebuildFromActive 增补——越界/重复活块由静默处理升级为报错（纵深防御第二道闸，
// 唯一调用方是崩溃恢复；见 P4.5M1 行为变更注 + P5M6-D18）。
TYPED_TEST(BlockAllocatorContractTest, RebuildRejectsOutOfRange) {
    std::vector<cabe::BlockId> active = { cabe::BlockId::Make(0, 7) };   // ≥ block_count(5)
    EXPECT_EQ(this->allocator_.RebuildFromActive(0, 5, active), cabe::err::kEngineBlockOutOfRange);
}

TYPED_TEST(BlockAllocatorContractTest, RebuildRejectsDuplicate) {
    std::vector<cabe::BlockId> active = {
        cabe::BlockId::Make(0, 2),
        cabe::BlockId::Make(0, 2),                                       // 两键声称同块
    };
    EXPECT_EQ(this->allocator_.RebuildFromActive(0, 5, active), cabe::err::kEngineDuplicateBlock);
}
