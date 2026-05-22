#include "common/structs.h"

#include <gtest/gtest.h>

#include <cstdint>

TEST(BlockId, MakeRoundTrip) {
    const auto b = cabe::BlockId::Make(0xAB, 0x123456789ull);
    EXPECT_EQ(static_cast<unsigned>(b.dev()), 0xABu);
    EXPECT_EQ(b.block_idx(), 0x123456789ull);
    EXPECT_EQ(b.byte_offset(), 0x123456789ull * cabe::kValueSize);
}

TEST(BlockId, Sizes) {
    EXPECT_EQ(sizeof(cabe::BlockId), 8u);
    EXPECT_EQ(sizeof(cabe::ValueMeta), 24u);
    EXPECT_EQ(alignof(cabe::ValueMeta), 8u);
}

TEST(BlockId, Comparison) {
    EXPECT_EQ(cabe::BlockId::Make(1, 2), cabe::BlockId::Make(1, 2));
    EXPECT_NE(cabe::BlockId::Make(1, 2), cabe::BlockId::Make(1, 3));
    EXPECT_LT(cabe::BlockId{1}, cabe::BlockId{2});
}

TEST(ValueMeta, ValueInitializedAllZero) {
    const cabe::ValueMeta vm{};
    EXPECT_EQ(vm.block.raw, std::uint64_t{0});
    EXPECT_EQ(vm.timestamp, std::uint64_t{0});
    EXPECT_EQ(vm.crc, std::uint32_t{0});
    EXPECT_EQ(vm.state, cabe::ValueState::Active);
    EXPECT_EQ(static_cast<int>(vm.reserved[0]), 0);
    EXPECT_EQ(static_cast<int>(vm.reserved[1]), 0);
    EXPECT_EQ(static_cast<int>(vm.reserved[2]), 0);
}

TEST(ValueState, Values) {
    EXPECT_EQ(static_cast<int>(cabe::ValueState::Active), 0);
    EXPECT_EQ(static_cast<int>(cabe::ValueState::Deleted), 1);
}
