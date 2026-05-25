#include "engine/meta_index.h"

#include <gtest/gtest.h>

namespace {

cabe::ValueMeta MakeMeta(std::uint64_t block_idx, std::uint32_t crc) {
    cabe::ValueMeta m{};
    m.block = cabe::BlockId::Make(0, block_idx);
    m.crc = crc;
    m.state = cabe::ValueState::Active;
    return m;
}

} // namespace

TEST(MetaIndex, InsertAndLookup) {
    cabe::MetaIndex idx;
    auto meta = MakeMeta(42, 0xDEAD);
    EXPECT_EQ(idx.Insert("key1", meta), cabe::err::kSuccess);

    cabe::ValueMeta out{};
    EXPECT_EQ(idx.Lookup("key1", &out), cabe::err::kSuccess);
    EXPECT_EQ(out.block.block_idx(), 42u);
    EXPECT_EQ(out.crc, 0xDEADu);
}

TEST(MetaIndex, LookupNotFound) {
    cabe::MetaIndex idx;
    cabe::ValueMeta out{};
    EXPECT_EQ(idx.Lookup("missing", &out), cabe::err::kIndexKeyNotFound);
}

TEST(MetaIndex, InsertOverwrites) {
    cabe::MetaIndex idx;
    idx.Insert("key1", MakeMeta(10, 0x1111));
    idx.Insert("key1", MakeMeta(20, 0x2222));

    cabe::ValueMeta out{};
    idx.Lookup("key1", &out);
    EXPECT_EQ(out.block.block_idx(), 20u);
    EXPECT_EQ(out.crc, 0x2222u);
}

TEST(MetaIndex, DeleteExisting) {
    cabe::MetaIndex idx;
    idx.Insert("key1", MakeMeta(1, 0));
    EXPECT_EQ(idx.Delete("key1"), cabe::err::kSuccess);

    cabe::ValueMeta out{};
    EXPECT_EQ(idx.Lookup("key1", &out), cabe::err::kIndexKeyNotFound);
}

TEST(MetaIndex, DeleteNotFound) {
    cabe::MetaIndex idx;
    EXPECT_EQ(idx.Delete("missing"), cabe::err::kIndexKeyNotFound);
}

TEST(MetaIndex, SizeAndContains) {
    cabe::MetaIndex idx;
    idx.Insert("k1", MakeMeta(1, 0));
    idx.Insert("k2", MakeMeta(2, 0));
    idx.Insert("k3", MakeMeta(3, 0));

    EXPECT_EQ(idx.Size(), 3u);
    EXPECT_TRUE(idx.Contains("k1"));
    EXPECT_TRUE(idx.Contains("k2"));
    EXPECT_FALSE(idx.Contains("missing"));
}

TEST(MetaIndex, MoveConstruct) {
    cabe::MetaIndex idx;
    idx.Insert("k1", MakeMeta(1, 0));
    idx.Insert("k2", MakeMeta(2, 0));

    cabe::MetaIndex idx2(std::move(idx));
    EXPECT_EQ(idx2.Size(), 2u);
    EXPECT_EQ(idx.Size(), 0u);
}
