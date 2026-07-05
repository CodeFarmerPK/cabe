#include "engine/engine.h"
#include "test/common/test_env.h"
#include "wal/wal_frame.h"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>

namespace {

using cabe::test::GetEnv;

class ValueBufferEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_ = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            GTEST_SKIP() << "需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE";
        }
    }

    void TearDown() override { engine_.Close(); }

    cabe::Options CreateOpts() const {
        cabe::Options opts;
        opts.devices.push_back({data_, wal_, snap_});
        opts.create = true;
        opts.snapshot_threshold_bytes = 1024 * 1024;
        return opts;
    }

    cabe::Engine engine_;
    std::string data_;
    std::string wal_;
    std::string snap_;
};

} // namespace

TEST(ValueBuffer, DefaultInvalid) {
    cabe::ValueBuffer buffer;
    EXPECT_FALSE(buffer.valid());
    EXPECT_TRUE(buffer.data().empty());
    EXPECT_TRUE(buffer.view().empty());
}

TEST(ValueBuffer, TypeTraits) {
    static_assert(!std::is_copy_constructible_v<cabe::ValueBuffer>);
    static_assert(!std::is_copy_assignable_v<cabe::ValueBuffer>);
    static_assert(std::is_move_constructible_v<cabe::ValueBuffer>);
    static_assert(std::is_move_assignable_v<cabe::ValueBuffer>);
}

TEST(ValueBuffer, MoveDefaultInvalid) {
    cabe::ValueBuffer source;
    cabe::ValueBuffer moved(std::move(source));
    EXPECT_FALSE(source.valid());
    EXPECT_FALSE(moved.valid());

    cabe::ValueBuffer assigned;
    assigned = std::move(moved);
    EXPECT_FALSE(moved.valid());
    EXPECT_FALSE(assigned.valid());
}

TEST(ValueBufferResult, OkDelegatesToStatus) {
    cabe::ValueBufferResult ok{cabe::Status::Ok(), {}};
    EXPECT_TRUE(ok.ok());

    cabe::ValueBufferResult failed{cabe::Status::Error(cabe::err::kEngineNotOpen), {}};
    EXPECT_FALSE(failed.ok());
}

TEST(Options, DefaultValueBufferPoolBlocks) {
    EXPECT_EQ(cabe::Options{}.value_buffer_pool_blocks, 16u);
}

TEST(EngineAllocateValueBuffer, NotOpen) {
    cabe::Engine engine;
    const auto r = engine.AllocateValueBuffer("k");
    EXPECT_EQ(r.status.code, cabe::err::kEngineNotOpen);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.buffer.valid());
}

TEST_F(ValueBufferEngineTest, EmptyKey) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());

    const auto r = engine_.AllocateValueBuffer("");
    EXPECT_EQ(r.status.code, cabe::err::kMemEmptyKey);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.buffer.valid());
}

TEST_F(ValueBufferEngineTest, KeyTooLong) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    const std::string too_long(cabe::kWalKeyMax + 1, 'x');

    const auto r = engine_.AllocateValueBuffer(too_long);
    EXPECT_EQ(r.status.code, cabe::err::kWalKeyTooLong);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.buffer.valid());
}

TEST_F(ValueBufferEngineTest, M1Placeholder) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());

    const auto r = engine_.AllocateValueBuffer("key");
    EXPECT_EQ(r.status.code, cabe::err::kEngineNotImplemented);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.buffer.valid());
}
