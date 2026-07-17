#include "engine/engine.h"
#include "test/common/test_env.h"
#include "util/hash.h"
#include "wal/wal_frame.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using cabe::test::GetEnv;

struct FreeDeleter {
    void operator()(std::byte* ptr) const noexcept {
        std::free(ptr);
    }
};

std::unique_ptr<std::byte, FreeDeleter> MakeAlignedValue(std::byte fill) {
    void* raw = nullptr;
    if (posix_memalign(&raw, 4096, cabe::kValueSize) != 0) return {};
    auto value = std::unique_ptr<std::byte, FreeDeleter>(static_cast<std::byte*>(raw));
    std::memset(value.get(), static_cast<int>(fill), cabe::kValueSize);
    return value;
}

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

TEST_F(ValueBufferEngineTest, ValueBufferSameKeyRoundTrip) {
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

TEST_F(ValueBufferEngineTest, ValueBufferDifferentKeyFallbackRoundTrip) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());

    auto r = engine_.AllocateValueBuffer("alloc-key");
    ASSERT_TRUE(r.ok());
    auto data = r.buffer.data();
    std::memset(data.data(), 0xB6, data.size());

    ASSERT_EQ(engine_.Put("put-key", r.buffer.view()).code, cabe::err::kSuccess);
    std::vector<std::byte> out(cabe::kValueSize);
    ASSERT_EQ(engine_.Get("put-key", cabe::DataBuffer{out}).code, cabe::err::kSuccess);
    EXPECT_EQ(out[0], std::byte{0xB6});
    EXPECT_EQ(out[cabe::kValueSize - 1], std::byte{0xB6});
}

TEST_F(ValueBufferEngineTest, ReleasedValueBufferViewFallbackRoundTrip) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());

    auto r = engine_.AllocateValueBuffer("released");
    ASSERT_TRUE(r.ok());
    auto data = r.buffer.data();
    std::memset(data.data(), 0xC3, data.size());
    const cabe::DataView view = r.buffer.view();
    r.buffer.reset();

    ASSERT_EQ(engine_.Put("released", view).code, cabe::err::kSuccess);
    std::vector<std::byte> out(cabe::kValueSize);
    ASSERT_EQ(engine_.Get("released", cabe::DataBuffer{out}).code, cabe::err::kSuccess);
    EXPECT_EQ(out[0], std::byte{0xC3});
    EXPECT_EQ(out[cabe::kValueSize - 1], std::byte{0xC3});
}

TEST_F(ValueBufferEngineTest, AlignedExternalValueRoundTrip) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());

    auto value = MakeAlignedValue(std::byte{0xD4});
    ASSERT_NE(value, nullptr);
    ASSERT_EQ(reinterpret_cast<std::uintptr_t>(value.get()) % 4096, 0u);

    ASSERT_EQ(engine_.Put("aligned", cabe::DataView{value.get(), cabe::kValueSize}).code,
              cabe::err::kSuccess);
    std::vector<std::byte> out(cabe::kValueSize);
    ASSERT_EQ(engine_.Get("aligned", cabe::DataBuffer{out}).code, cabe::err::kSuccess);
    EXPECT_EQ(out[0], std::byte{0xD4});
    EXPECT_EQ(out[cabe::kValueSize - 1], std::byte{0xD4});
}

TEST_F(ValueBufferEngineTest, ConcurrentMixedPutSources) {
    auto opts = CreateOpts();
    opts.value_buffer_pool_blocks = 8;
    ASSERT_TRUE(engine_.Open(opts).ok());

    constexpr int kThreads = 4;
    constexpr int kIters = 12;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int tid = 0; tid < kThreads; ++tid) {
        threads.emplace_back([&, tid] {
            std::vector<std::byte> out(cabe::kValueSize);
            for (int i = 0; i < kIters; ++i) {
                const std::string key = "mixed_" + std::to_string(tid) + "_" + std::to_string(i);
                const std::byte fill = static_cast<std::byte>((tid * 31 + i) & 0xFF);
                if (i % 3 == 0) {
                    auto r = engine_.AllocateValueBuffer(key);
                    ASSERT_TRUE(r.ok()) << r.status.code;
                    std::memset(r.buffer.data().data(), static_cast<int>(fill), cabe::kValueSize);
                    EXPECT_EQ(engine_.Put(key, r.buffer.view()).code, cabe::err::kSuccess);
                } else if (i % 3 == 1) {
                    auto value = MakeAlignedValue(fill);
                    ASSERT_NE(value, nullptr);
                    EXPECT_EQ(engine_.Put(key, cabe::DataView{value.get(), cabe::kValueSize}).code,
                              cabe::err::kSuccess);
                } else {
                    std::vector<std::byte> value(cabe::kValueSize, fill);
                    EXPECT_EQ(engine_.Put(key, cabe::DataView{value}).code, cabe::err::kSuccess);
                }

                EXPECT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kSuccess);
                EXPECT_EQ(out[0], fill);
                EXPECT_EQ(out[cabe::kValueSize - 1], fill);
            }
        });
    }

    for (auto& t : threads) t.join();
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

TEST_F(ValueBufferEngineTest, CloseRejectsOperationsAndAllocation) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    ASSERT_TRUE(engine_.Close().ok());

    std::vector<std::byte> value(cabe::kValueSize);
    EXPECT_EQ(engine_.Put("closed", cabe::DataView{value}).code, cabe::err::kEngineNotOpen);
    EXPECT_EQ(engine_.Get("closed", cabe::DataBuffer{value}).code, cabe::err::kEngineNotOpen);
    EXPECT_EQ(engine_.Delete("closed").code, cabe::err::kEngineNotOpen);
    EXPECT_EQ(engine_.AllocateValueBuffer("closed").status.code, cabe::err::kEngineNotOpen);
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

TEST_F(ValueBufferMultiDeviceTest, OtherDeviceValueBufferFallbackRoundTrip) {
    const std::string key0 = KeyForDevice(0);
    const std::string key1 = KeyForDevice(1);
    ASSERT_FALSE(key0.empty());
    ASSERT_FALSE(key1.empty());
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());

    auto r = engine_.AllocateValueBuffer(key0);
    ASSERT_TRUE(r.ok()) << r.status.code;
    auto data = r.buffer.data();
    std::memset(data.data(), 0xE5, data.size());

    ASSERT_EQ(engine_.Put(key1, r.buffer.view()).code, cabe::err::kSuccess);
    std::vector<std::byte> out(cabe::kValueSize);
    ASSERT_EQ(engine_.Get(key1, cabe::DataBuffer{out}).code, cabe::err::kSuccess);
    EXPECT_EQ(out[0], std::byte{0xE5});
    EXPECT_EQ(out[cabe::kValueSize - 1], std::byte{0xE5});
}
