#include "engine/buffer_pool.h"

#include <gtest/gtest.h>

#include <cstdint>

TEST(BufferPool, AllocateAndFree) {
    cabe::BufferPool pool(2);
    EXPECT_EQ(pool.capacity(), 2u);
    EXPECT_EQ(pool.available(), 2u);

    auto* buf = pool.Allocate();
    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(pool.available(), 1u);

    pool.Free(buf);
    EXPECT_EQ(pool.available(), 2u);

    auto* buf2 = pool.Allocate();
    EXPECT_EQ(buf2, buf);
    pool.Free(buf2);
}

TEST(BufferPool, AllocateAll) {
    cabe::BufferPool pool(cabe::kDefaultPoolBlocks);
    std::vector<std::byte*> bufs;
    for (std::size_t i = 0; i < cabe::kDefaultPoolBlocks; ++i) {
        auto* b = pool.Allocate();
        ASSERT_NE(b, nullptr) << "i=" << i;
        bufs.push_back(b);
    }
    EXPECT_EQ(pool.available(), 0u);
    EXPECT_EQ(pool.Allocate(), nullptr);

    for (auto* b : bufs) pool.Free(b);
    EXPECT_EQ(pool.available(), cabe::kDefaultPoolBlocks);
}

TEST(BufferPool, FreeRestoresAvailable) {
    cabe::BufferPool pool(4);
    auto* a = pool.Allocate();
    auto* b = pool.Allocate();
    EXPECT_EQ(pool.available(), 2u);
    pool.Free(a);
    EXPECT_EQ(pool.available(), 3u);
    pool.Free(b);
    EXPECT_EQ(pool.available(), 4u);
}

TEST(BufferPool, Alignment) {
    cabe::BufferPool pool(4);
    for (int i = 0; i < 4; ++i) {
        auto* buf = pool.Allocate();
        ASSERT_NE(buf, nullptr);
        auto addr = reinterpret_cast<std::uintptr_t>(buf);
        EXPECT_EQ(addr % cabe::kPageSize, 0u) << "buf " << i << " not aligned";
        pool.Free(buf);
    }
}

TEST(BufferPool, MoveConstruct) {
    cabe::BufferPool pool(4);
    auto* buf = pool.Allocate();
    ASSERT_NE(buf, nullptr);
    pool.Free(buf);

    cabe::BufferPool pool2(std::move(pool));
    EXPECT_EQ(pool2.capacity(), 4u);
    EXPECT_EQ(pool2.available(), 4u);
    EXPECT_EQ(pool.capacity(), 0u);
    EXPECT_EQ(pool.available(), 0u);
}

TEST(BufferPool, DestructorWarnsOnLeak) {
    {
        cabe::BufferPool pool(2);
        pool.Allocate();
    }
}
