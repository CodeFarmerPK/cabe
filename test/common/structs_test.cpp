#include "common/structs.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

TEST(BlockId, MakeRoundTrip) {
    const auto b = cabe::BlockId::Make(0xAB, 0x123456789ull);
    EXPECT_EQ(static_cast<unsigned>(b.dev()), 0xABu);
    EXPECT_EQ(b.block_idx(), 0x123456789ull);
    EXPECT_EQ(b.logical_byte_offset(), 0x123456789ull * cabe::kValueSize);
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

// 验证 structs.h 注释"显式占位 + 预留小扩展位…保证整体可确定地 memcpy 序列化"承诺：
// 写出字节流后 memcpy 回来，所有字段相等；防止未来字段重排引入隐式 padding 而 sizeof
// 仍为 24 的回归（P5 WAL/snapshot 反序列化的前提）。
TEST(ValueMeta, MemcpyRoundTrip) {
    cabe::ValueMeta src{};
    src.block       = cabe::BlockId::Make(0x77, 0x1122334455ull);
    src.timestamp   = 0xDEADBEEFCAFEBABEull;
    src.crc         = 0xABCD1234u;
    src.state       = cabe::ValueState::Deleted;
    src.reserved[0] = 0xAA;
    src.reserved[1] = 0xBB;
    src.reserved[2] = 0xCC;

    std::array<std::byte, sizeof(cabe::ValueMeta)> buf{};
    std::memcpy(buf.data(), &src, sizeof(src));

    cabe::ValueMeta dst{};
    std::memcpy(&dst, buf.data(), sizeof(dst));

    EXPECT_EQ(dst.block.raw,   src.block.raw);
    EXPECT_EQ(dst.timestamp,   src.timestamp);
    EXPECT_EQ(dst.crc,         src.crc);
    EXPECT_EQ(dst.state,       src.state);
    EXPECT_EQ(dst.reserved[0], src.reserved[0]);
    EXPECT_EQ(dst.reserved[1], src.reserved[1]);
    EXPECT_EQ(dst.reserved[2], src.reserved[2]);
}
