/*
 * Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 *
 * cabe::Engine（P2 公开 API）线程安全测试。
 *
 * engine_thread_test.cpp 测的是内部 ::Engine。本文件验证 P2 增加的层
 * （Pimpl 转发 / TranslateStatus / 类型适配 / 便捷重载）在并发下不引入
 * 新的数据竞争或语义错误。
 *
 * 覆盖：
 *   1. 多线程并发 Get（shared_lock 真并发，公开 API 路径）
 *   2. 并发 Put 不同 key
 *   3. 并发 Put + Get 同 key
 *   4. 并发 Delete + Get
 *   5. 并发 Size + 写入
 *   6. 便捷重载 Put(k,v) 与三参 Put({},k,v) 的混合并发
 *
 * 不覆盖（cabe::Engine 不暴露此能力或场景不现实）：
 *   - 并发 Open 同一 path（已知限制，路线图 P3 加 LOCK 文件）
 *   - 析构与活跃调用并发（API 头文档要求调用方保证）
 *
 * TSAN（cmake -DCABE_ENABLE_TSAN=ON）下意义最大。
 */

#include <gtest/gtest.h>
#include "cabe/cabe.h"
#include "common/structs.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <thread>
#include <vector>

namespace {

    // 裸设备语义:读 CABE_TEST_DEVICE 环境变量。空字符串视作未设置。
    std::string GetTestDevice() {
        const char* env = std::getenv("CABE_TEST_DEVICE");
        if (env == nullptr || *env == '\0') return {};
        return env;
}

class CabeEngineThreadTest : public ::testing::Test {
protected:
    std::unique_ptr<cabe::Engine> engine_;
    std::string                   devicePath_;

    void SetUp() override {
        devicePath_ = GetTestDevice();
        if (devicePath_.empty()) {
            GTEST_SKIP() << "CABE_TEST_DEVICE not set; "
                            "use scripts/mkloop.sh to create a loop device "
                            "and `export CABE_TEST_DEVICE=/dev/loopX`";
        }
        cabe::Options opts;
        opts.device_path       = devicePath_;
        opts.buffer_pool_count = 8;
        ASSERT_TRUE(cabe::Engine::Open(opts, &engine_).ok());
    }

    void TearDown() override {
        if (engine_) {
            (void) engine_->Close();
            engine_.reset();
        }
        // 不 unlink:裸设备节点由 sysadmin / mkloop.sh 管理
    }

    static std::vector<std::byte> MakeBytes(size_t n, uint8_t fill) {
        return std::vector<std::byte>(n, static_cast<std::byte>(fill));
    }
};

// ============================================================
// 1. 并发 Get（shared_lock 真并发）
// ============================================================
TEST_F(CabeEngineThreadTest, ConcurrentGet) {
    const auto value = MakeBytes(CABE_VALUE_DATA_SIZE, 0x47);
    ASSERT_TRUE(engine_->Put("g_key", value).ok());

    constexpr int kThreads = 6;
    constexpr int kIters   = 25;
    std::latch start(kThreads);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            std::vector<std::byte> out;
            for (int i = 0; i < kIters; ++i) {
                const cabe::Status s = engine_->Get("g_key", &out);
                if (!s.ok() || out.size() != CABE_VALUE_DATA_SIZE) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                for (auto b : out) {
                    if (b != static_cast<std::byte>(0x47)) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(0, errors.load());
}

// ============================================================
// 2. 并发 Put 不同 key
// ============================================================
TEST_F(CabeEngineThreadTest, ConcurrentPutDifferentKeys) {
    constexpr int kThreads = 6;
    constexpr int kIters   = 4;
    std::latch start(kThreads);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            const std::string key  = "p_" + std::to_string(t);
            const auto        data = MakeBytes(CABE_VALUE_DATA_SIZE,
                                                static_cast<uint8_t>('A' + t));
            for (int i = 0; i < kIters; ++i) {
                if (!engine_->Put(key, data).ok()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(0, errors.load());

    // 最终验证
    for (int t = 0; t < kThreads; ++t) {
        const std::string key = "p_" + std::to_string(t);
        const auto fill = static_cast<std::byte>('A' + t);
        std::vector<std::byte> out;
        ASSERT_TRUE(engine_->Get(key, &out).ok()) << "key=" << key;
        ASSERT_EQ(out.size(), CABE_VALUE_DATA_SIZE);
        for (auto b : out) {
            EXPECT_EQ(fill, b) << "key=" << key;
            if (b != fill) break;
        }
    }
}

// ============================================================
// 3. 并发 Put + Get 同 key（读不应见中间态）
// ============================================================
TEST_F(CabeEngineThreadTest, ConcurrentPutGetSameKey) {
    auto initData = MakeBytes(CABE_VALUE_DATA_SIZE, 0x01);
    ASSERT_TRUE(engine_->Put("rw_key", initData).ok());

    constexpr int kReaders     = 4;
    constexpr int kWriterIters = 30;
    constexpr int kReaderIters = 50;
    std::latch start(kReaders + 1);
    std::atomic<int> readErrors{0};
    std::vector<std::thread> threads;
    threads.reserve(kReaders + 1);

    threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < kWriterIters; ++i) {
            auto data = MakeBytes(CABE_VALUE_DATA_SIZE,
                                   static_cast<uint8_t>('A' + (i % 26)));
            (void) engine_->Put("rw_key", data);
        }
    });

    for (int t = 0; t < kReaders; ++t) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            std::vector<std::byte> out;
            for (int i = 0; i < kReaderIters; ++i) {
                const cabe::Status s = engine_->Get("rw_key", &out);
                if (!s.ok() || out.size() != CABE_VALUE_DATA_SIZE) {
                    readErrors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const auto first = out[0];
                for (size_t b = 1; b < out.size(); ++b) {
                    if (out[b] != first) {
                        readErrors.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(0, readErrors.load());
}

// ============================================================
// 4. 并发 Delete + Get（NotFound 是合法返回，其他错误码不允许）
// ============================================================
TEST_F(CabeEngineThreadTest, ConcurrentDeleteAndGet) {
    constexpr int kReaders       = 3;
    constexpr int kDeleterIters  = 25;
    constexpr int kReaderIters   = 60;
    auto data = MakeBytes(CABE_VALUE_DATA_SIZE / 2, 0xDD);
    ASSERT_TRUE(engine_->Put("dg", data).ok());

    std::latch start(kReaders + 1);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kReaders + 1);

    threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < kDeleterIters; ++i) {
            (void) engine_->Put("dg", data);
            (void) engine_->Delete("dg");
        }
        (void) engine_->Put("dg", data);
    });

    for (int t = 0; t < kReaders; ++t) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            std::vector<std::byte> out;
            for (int i = 0; i < kReaderIters; ++i) {
                const cabe::Status s = engine_->Get("dg", &out);
                if (!s.ok() && !s.IsNotFound()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (s.ok()) {
                    if (out.size() != CABE_VALUE_DATA_SIZE / 2) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    for (auto b : out) {
                        if (b != static_cast<std::byte>(0xDD)) {
                            errors.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                } else {
                    // NotFound：API 契约要求 *out 被清空
                    EXPECT_EQ(0u, out.size());
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(0, errors.load());
}

// ============================================================
// 5. 并发 Size + 写入（shared_lock vs unique_lock 的混合）
// ============================================================
TEST_F(CabeEngineThreadTest, ConcurrentSizeAndPut) {
    constexpr int kSizeReaders = 4;
    constexpr int kKeys        = 6;
    constexpr int kReaderIters = 50;

    std::latch start(kSizeReaders + 1);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kSizeReaders + 1);

    // writer：循环 Put kKeys 个不同 key
    threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < kKeys; ++i) {
            const std::string key = "sz_" + std::to_string(i);
            auto data = MakeBytes(512, static_cast<uint8_t>('A' + i));
            (void) engine_->Put(key, data);
        }
    });

    // size readers
    for (int t = 0; t < kSizeReaders; ++t) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            for (int i = 0; i < kReaderIters; ++i) {
                const size_t sz = engine_->Size();
                if (sz > static_cast<size_t>(kKeys)) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(0, errors.load());
    EXPECT_EQ(static_cast<size_t>(kKeys), engine_->Size());
}

// ============================================================
// 6. 便捷重载与三参重载并发混用（验证 Put({}, k, v) 与 Put(k, v) 等价）
// ============================================================
TEST_F(CabeEngineThreadTest, MixedOverloadCalls) {
    constexpr int kThreads = 4;
    constexpr int kIters   = 6;
    std::latch start(kThreads);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            const std::string key  = "ov_" + std::to_string(t);
            const auto        data = MakeBytes(CABE_VALUE_DATA_SIZE,
                                                static_cast<uint8_t>('A' + t));
            for (int i = 0; i < kIters; ++i) {
                // 偶数走便捷重载，奇数走三参重载
                const cabe::Status s = (i % 2 == 0)
                    ? engine_->Put(key, data)
                    : engine_->Put({}, key, data);
                if (!s.ok()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(0, errors.load());

    // 验证：每个 key 都能用便捷重载读出，内容是对应 fill
    for (int t = 0; t < kThreads; ++t) {
        const std::string key = "ov_" + std::to_string(t);
        const auto fill = static_cast<std::byte>('A' + t);
        std::vector<std::byte> out;
        ASSERT_TRUE(engine_->Get(key, &out).ok());
        ASSERT_EQ(CABE_VALUE_DATA_SIZE, out.size());
        for (auto b : out) {
            EXPECT_EQ(fill, b);
            if (b != fill) break;
        }
    }
}

} // namespace
