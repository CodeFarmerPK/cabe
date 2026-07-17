#include "engine/engine.h"

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

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

TEST(ValueBuffer, ResetDefaultInvalid) {
    cabe::ValueBuffer buffer;
    buffer.reset();
    EXPECT_FALSE(buffer.valid());
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
    const auto result = engine.AllocateValueBuffer("k");
    EXPECT_EQ(result.status.code, cabe::err::kEngineNotOpen);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.buffer.valid());
}
