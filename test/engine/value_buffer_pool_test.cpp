#include "engine/value_buffer_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr cabe::DeviceId kDevice = 0;
constexpr std::uint64_t kPoolId = 42;

std::shared_ptr<cabe::ValueBufferPool> MakePool(std::size_t slots) {
    auto r = cabe::ValueBufferPool::Create(kDevice, kPoolId, slots);
    EXPECT_TRUE(r.ok()) << "code=" << r.status.code;
    return r.pool;
}

} // namespace

TEST(ValueBufferPool, CreateZeroCapacity) {
    auto pool = MakePool(0);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(pool->slot_count(), 0u);

    auto r = pool->Allocate();
    EXPECT_EQ(r.status.code, cabe::err::kEnginePoolExhausted);
    EXPECT_FALSE(r.buffer.valid());

    pool->BeginClose();
    pool->WaitUntilIdle();
}

TEST(ValueBufferPool, CreateAlignedSlab) {
    auto pool = MakePool(4);
    std::vector<cabe::ValueBuffer> buffers;
    for (int i = 0; i < 4; ++i) {
        auto r = pool->Allocate();
        ASSERT_TRUE(r.ok()) << "i=" << i << " code=" << r.status.code;
        ASSERT_TRUE(r.buffer.valid());
        const auto addr = reinterpret_cast<std::uintptr_t>(r.buffer.view().data());
        EXPECT_EQ(addr % cabe::kValueSize, 0u);
        EXPECT_EQ(r.buffer.view().size(), cabe::kValueSize);
        EXPECT_EQ(r.buffer.data().size(), cabe::kValueSize);
        buffers.push_back(std::move(r.buffer));
    }
}

TEST(ValueBufferPool, ExhaustionAndReuse) {
    auto pool = MakePool(2);
    auto a = pool->Allocate();
    auto b = pool->Allocate();
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());

    auto exhausted = pool->Allocate();
    EXPECT_EQ(exhausted.status.code, cabe::err::kEnginePoolExhausted);

    a.buffer.reset();
    auto c = pool->Allocate();
    EXPECT_TRUE(c.ok());
    EXPECT_TRUE(c.buffer.valid());
}

TEST(ValueBufferPool, MoveReleasesOnce) {
    auto pool = MakePool(1);
    auto r = pool->Allocate();
    ASSERT_TRUE(r.ok());

    cabe::ValueBuffer moved(std::move(r.buffer));
    EXPECT_FALSE(r.buffer.valid());
    ASSERT_TRUE(moved.valid());

    cabe::ValueBuffer assigned;
    assigned = std::move(moved);
    EXPECT_FALSE(moved.valid());
    ASSERT_TRUE(assigned.valid());

    assigned.reset();
    EXPECT_FALSE(assigned.valid());
    EXPECT_TRUE(pool->Allocate().ok());
}

TEST(ValueBufferPool, IdentifyAllocatedSlot) {
    auto pool = MakePool(1);
    auto r = pool->Allocate();
    ASSERT_TRUE(r.ok());

    const auto info = pool->Identify(r.buffer.view());
    EXPECT_TRUE(info.matched);
    EXPECT_EQ(info.device_id, kDevice);
    EXPECT_EQ(info.pool_id, kPoolId);
    EXPECT_EQ(info.slot_index, 0u);
}

TEST(ValueBufferPool, IdentifyRejectsWrongAddressSizeAndReleasedSlot) {
    auto pool = MakePool(1);
    auto r = pool->Allocate();
    ASSERT_TRUE(r.ok());

    const auto view = r.buffer.view();
    EXPECT_FALSE(pool->Identify(cabe::DataView{view.data() + 1, cabe::kValueSize}).matched);
    EXPECT_FALSE(pool->Identify(cabe::DataView{view.data(), cabe::kValueSize - 1}).matched);

    std::vector<std::byte> external(cabe::kValueSize);
    EXPECT_FALSE(pool->Identify(cabe::DataView{external}).matched);

    r.buffer.reset();
    EXPECT_FALSE(pool->Identify(view).matched);
}

TEST(ValueBufferPool, BeginCloseRejectsAllocate) {
    auto pool = MakePool(1);
    pool->BeginClose();

    auto r = pool->Allocate();
    EXPECT_EQ(r.status.code, cabe::err::kEngineNotOpen);
    pool->WaitUntilIdle();
}

TEST(ValueBufferPool, WaitUntilIdleBlocksUntilRelease) {
    auto pool = MakePool(1);
    auto r = pool->Allocate();
    ASSERT_TRUE(r.ok());
    pool->BeginClose();

    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    std::thread waiter([&] {
        entered.store(true, std::memory_order_release);
        pool->WaitUntilIdle();
        returned.store(true, std::memory_order_release);
    });

    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(returned.load(std::memory_order_acquire));

    r.buffer.reset();
    waiter.join();
    EXPECT_TRUE(returned.load(std::memory_order_acquire));
}

TEST(ValueBufferPool, ConcurrentAllocateRelease) {
    constexpr std::size_t kSlots = 8;
    auto pool = MakePool(kSlots);
    auto active = std::make_unique<std::atomic<int>[]>(kSlots);
    for (std::size_t i = 0; i < kSlots; ++i) active[i].store(0, std::memory_order_relaxed);

    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kIters = 200;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (std::size_t tid = 0; tid < kThreads; ++tid) {
        threads.emplace_back([&, tid] {
            for (std::size_t i = 0; i < kIters; ++i) {
                auto r = pool->Allocate();
                if (!r.ok()) {
                    EXPECT_EQ(r.status.code, cabe::err::kEnginePoolExhausted);
                    failures.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                    continue;
                }

                const auto info = pool->Identify(r.buffer.view());
                ASSERT_TRUE(info.matched);
                ASSERT_LT(info.slot_index, kSlots);
                EXPECT_EQ(active[info.slot_index].fetch_add(1, std::memory_order_acq_rel), 0);
                r.buffer.data()[0] = static_cast<std::byte>((tid + i) & 0xFF);
                EXPECT_EQ(active[info.slot_index].fetch_sub(1, std::memory_order_acq_rel), 1);
                r.buffer.reset();
            }
        });
    }
    for (auto& t : threads) t.join();

    for (std::size_t i = 0; i < kSlots; ++i) {
        EXPECT_EQ(active[i].load(std::memory_order_acquire), 0);
    }
    EXPECT_GE(failures.load(std::memory_order_relaxed), 0);
}
