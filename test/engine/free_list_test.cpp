#include "engine/free_list.h"

#include <gtest/gtest.h>

#include <vector>

TEST(FreeList, InitFillsAll) {
    cabe::FreeList fl;
    fl.Init(0, 100);
    EXPECT_EQ(fl.available(), 100u);
    EXPECT_FALSE(fl.empty());
}

TEST(FreeList, AllocateReturnsSequential) {
    cabe::FreeList fl;
    fl.Init(0, 5);

    for (std::uint64_t i = 0; i < 5; ++i) {
        cabe::BlockId bid{};
        int32_t rc = fl.Allocate(&bid);
        EXPECT_EQ(rc, cabe::err::kSuccess);
        EXPECT_EQ(bid.block_idx(), i) << "第 " << i << " 次分配";
        EXPECT_EQ(bid.dev(), 0);
    }
    EXPECT_TRUE(fl.empty());
}

TEST(FreeList, AllocateExhausted) {
    cabe::FreeList fl;
    fl.Init(0, 3);

    cabe::BlockId bid{};
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(fl.Allocate(&bid), cabe::err::kSuccess);
    }
    EXPECT_EQ(fl.Allocate(&bid), cabe::err::kEngineNoSpace);
}

TEST(FreeList, FreeRestores) {
    cabe::FreeList fl;
    fl.Init(0, 2);

    cabe::BlockId bid{};
    fl.Allocate(&bid);
    EXPECT_EQ(fl.available(), 1u);

    fl.Free(bid);
    EXPECT_EQ(fl.available(), 2u);

    cabe::BlockId bid2{};
    fl.Allocate(&bid2);
    EXPECT_EQ(bid2.raw, bid.raw);
}

TEST(FreeList, DeviceIdPreserved) {
    cabe::FreeList fl;
    fl.Init(5, 3);

    cabe::BlockId bid{};
    fl.Allocate(&bid);
    EXPECT_EQ(bid.dev(), 5);
}

TEST(FreeList, MoveConstruct) {
    cabe::FreeList fl;
    fl.Init(0, 10);

    cabe::BlockId bid{};
    fl.Allocate(&bid);

    cabe::FreeList fl2(std::move(fl));
    EXPECT_EQ(fl2.available(), 9u);
    EXPECT_TRUE(fl.empty());
}
