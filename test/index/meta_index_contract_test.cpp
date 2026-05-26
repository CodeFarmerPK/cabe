#include "index/hash/hash_meta_index.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

cabe::ValueMeta MakeMeta(std::uint64_t block_idx, std::uint32_t crc) {
    cabe::ValueMeta m{};
    m.block = cabe::BlockId::Make(0, block_idx);
    m.crc = crc;
    m.state = cabe::ValueState::Active;
    return m;
}

} // namespace

template<typename T>
class MetaIndexContractTest : public ::testing::Test {
protected:
    T index_;
};

using MetaIndexImpls = ::testing::Types<cabe::HashMetaIndex>;
TYPED_TEST_SUITE(MetaIndexContractTest, MetaIndexImpls);

TYPED_TEST(MetaIndexContractTest, InsertAndLookup) {
    auto meta = MakeMeta(42, 0xDEAD);
    EXPECT_EQ(this->index_.Insert("key1", meta), cabe::err::kSuccess);

    cabe::ValueMeta out{};
    EXPECT_EQ(this->index_.Lookup("key1", &out), cabe::err::kSuccess);
    EXPECT_EQ(out.block.block_idx(), 42u);
    EXPECT_EQ(out.crc, 0xDEADu);
}

TYPED_TEST(MetaIndexContractTest, LookupNotFound) {
    cabe::ValueMeta out{};
    EXPECT_EQ(this->index_.Lookup("missing", &out), cabe::err::kIndexKeyNotFound);
}

TYPED_TEST(MetaIndexContractTest, InsertOverwrites) {
    this->index_.Insert("key1", MakeMeta(10, 0x1111));
    this->index_.Insert("key1", MakeMeta(20, 0x2222));

    cabe::ValueMeta out{};
    this->index_.Lookup("key1", &out);
    EXPECT_EQ(out.block.block_idx(), 20u);
    EXPECT_EQ(out.crc, 0x2222u);
}

TYPED_TEST(MetaIndexContractTest, DeleteExisting) {
    this->index_.Insert("key1", MakeMeta(1, 0));
    EXPECT_EQ(this->index_.Delete("key1"), cabe::err::kSuccess);

    cabe::ValueMeta out{};
    EXPECT_EQ(this->index_.Lookup("key1", &out), cabe::err::kIndexKeyNotFound);
}

TYPED_TEST(MetaIndexContractTest, DeleteNotFound) {
    EXPECT_EQ(this->index_.Delete("missing"), cabe::err::kIndexKeyNotFound);
}

TYPED_TEST(MetaIndexContractTest, SizeAndContains) {
    this->index_.Insert("k1", MakeMeta(1, 0));
    this->index_.Insert("k2", MakeMeta(2, 0));
    this->index_.Insert("k3", MakeMeta(3, 0));

    EXPECT_EQ(this->index_.Size(), 3u);
    EXPECT_TRUE(this->index_.Contains("k1"));
    EXPECT_TRUE(this->index_.Contains("k2"));
    EXPECT_FALSE(this->index_.Contains("missing"));
}

TYPED_TEST(MetaIndexContractTest, ForEachVisitsAll) {
    this->index_.Insert("a", MakeMeta(1, 0));
    this->index_.Insert("b", MakeMeta(2, 0));
    this->index_.Insert("c", MakeMeta(3, 0));

    std::vector<std::string> keys;
    this->index_.ForEach([&](std::string_view key, const cabe::ValueMeta&) {
        keys.emplace_back(key);
    });
    EXPECT_EQ(keys.size(), 3u);
}

TYPED_TEST(MetaIndexContractTest, WriteSnapshotStub) {
    EXPECT_EQ(this->index_.WriteSnapshot("/tmp/dummy"), cabe::err::kEngineNotImplemented);
}

TYPED_TEST(MetaIndexContractTest, LoadSnapshotStub) {
    EXPECT_EQ(this->index_.LoadSnapshot("/tmp/dummy"), cabe::err::kEngineNotImplemented);
}

TYPED_TEST(MetaIndexContractTest, ConceptSatisfied) {
    static_assert(cabe::MetaIndexBackend<TypeParam>);
}
