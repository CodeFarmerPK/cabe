#include "engine/engine.h"
#include "test/common/test_env.h"
#include "util/hash.h"
#include "wal/wal_frame.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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

class ValueBufferMultiDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        d1_ = GetEnv("CABE_TEST_DEVICE");   w1_ = GetEnv("CABE_TEST_WAL_DEVICE");   s1_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        d2_ = GetEnv("CABE_TEST_DEVICE2");  w2_ = GetEnv("CABE_TEST_WAL_DEVICE2");  s2_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE2");
        if (d1_.empty() || w1_.empty() || s1_.empty() || d2_.empty() || w2_.empty() || s2_.empty()) {
            GTEST_SKIP() << "需要两组 loop 设备(CABE_TEST_* + CABE_TEST_*2)";
        }
    }

    void TearDown() override { engine_.Close(); }

    cabe::Options CreateOpts() const {
        cabe::Options opts;
        opts.devices.push_back({d1_, w1_, s1_});
        opts.devices.push_back({d2_, w2_, s2_});
        opts.create = true;
        opts.snapshot_threshold_bytes = 1024 * 1024;
        opts.value_buffer_pool_blocks = 1;
        return opts;
    }

    std::string KeyForDevice(cabe::DeviceId device) const {
        for (int i = 0; i < 1000; ++i) {
            std::string key = "vbmd" + std::to_string(device) + "_" + std::to_string(i);
            if (cabe::util::RouteToDevice(key, 2) == device) return key;
        }
        return {};
    }

    cabe::Engine engine_;
    std::string d1_, w1_, s1_, d2_, w2_, s2_;
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

TEST_F(ValueBufferEngineTest, AllocatesValidBuffer) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());

    const auto r = engine_.AllocateValueBuffer("key");
    EXPECT_EQ(r.status.code, cabe::err::kSuccess);
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.buffer.valid());
    EXPECT_EQ(r.buffer.view().size(), cabe::kValueSize);
    const auto addr = reinterpret_cast<std::uintptr_t>(r.buffer.view().data());
    EXPECT_EQ(addr % cabe::kValueSize, 0u);
}

TEST_F(ValueBufferEngineTest, ZeroCapacity) {
    auto opts = CreateOpts();
    opts.value_buffer_pool_blocks = 0;
    ASSERT_TRUE(engine_.Open(opts).ok());

    const auto r = engine_.AllocateValueBuffer("key");
    EXPECT_EQ(r.status.code, cabe::err::kEnginePoolExhausted);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.buffer.valid());
}

TEST_F(ValueBufferEngineTest, PoolExhaustion) {
    auto opts = CreateOpts();
    opts.value_buffer_pool_blocks = 1;
    ASSERT_TRUE(engine_.Open(opts).ok());

    auto first = engine_.AllocateValueBuffer("key");
    ASSERT_TRUE(first.ok());
    auto second = engine_.AllocateValueBuffer("key");
    EXPECT_EQ(second.status.code, cabe::err::kEnginePoolExhausted);

    first.buffer.reset();
    EXPECT_TRUE(engine_.AllocateValueBuffer("key").ok());
}

TEST_F(ValueBufferEngineTest, ValueBufferPutGetRoundTripUsesExistingCopyPath) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());

    auto r = engine_.AllocateValueBuffer("round");
    ASSERT_TRUE(r.ok());
    auto data = r.buffer.data();
    std::memset(data.data(), 0xA7, data.size());

    ASSERT_EQ(engine_.Put("round", r.buffer.view()).code, cabe::err::kSuccess);
    std::vector<std::byte> out(cabe::kValueSize);
    ASSERT_EQ(engine_.Get("round", cabe::DataBuffer{out}).code, cabe::err::kSuccess);
    EXPECT_EQ(out[0], std::byte{0xA7});
    EXPECT_EQ(out[cabe::kValueSize - 1], std::byte{0xA7});
}

TEST_F(ValueBufferEngineTest, CloseWaitsForOutstandingValueBuffer) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    auto r = engine_.AllocateValueBuffer("held");
    ASSERT_TRUE(r.ok());

    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    cabe::Status close_status = cabe::Status::Error(cabe::err::kEngineNotOpen);
    std::thread closer([&] {
        entered.store(true, std::memory_order_release);
        close_status = engine_.Close();
        returned.store(true, std::memory_order_release);
    });

    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(returned.load(std::memory_order_acquire));

    r.buffer.reset();
    closer.join();
    EXPECT_TRUE(close_status.ok()) << "code=" << close_status.code;
    EXPECT_TRUE(returned.load(std::memory_order_acquire));
}

TEST_F(ValueBufferMultiDeviceTest, PoolsAreIndependentPerDevice) {
    const std::string key0 = KeyForDevice(0);
    const std::string key1 = KeyForDevice(1);
    ASSERT_FALSE(key0.empty());
    ASSERT_FALSE(key1.empty());
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());

    auto a = engine_.AllocateValueBuffer(key0);
    auto b = engine_.AllocateValueBuffer(key1);
    ASSERT_TRUE(a.ok()) << a.status.code;
    ASSERT_TRUE(b.ok()) << b.status.code;

    EXPECT_EQ(engine_.AllocateValueBuffer(key0).status.code, cabe::err::kEnginePoolExhausted);
    EXPECT_EQ(engine_.AllocateValueBuffer(key1).status.code, cabe::err::kEnginePoolExhausted);

    a.buffer.reset();
    EXPECT_TRUE(engine_.AllocateValueBuffer(key0).ok());
    EXPECT_EQ(engine_.AllocateValueBuffer(key1).status.code, cabe::err::kEnginePoolExhausted);
}
