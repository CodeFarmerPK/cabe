/*
 * Project: Cabe
 * Created Time: 2026-04-21
 * Created by: CodeFarmerPK
 *
 * Engine 线程安全测试（P1.2）
 *
 * 覆盖场景：
 *   1. 多线程并发 Get（shared_lock 允许真并发读）
 *   2. 并发 Put 不同 key，结果全部可见且内容正确
 *   3. 并发 Put 同一 key（last-writer-wins，无撕裂）
 *   4. 并发 Put + Get 混合（读写互斥，读不见中间态）
 *   5. 并发 Put→Delete→Remove 生命周期（无状态损坏）
 *   6. 并发多 chunk 写入与读取
 *   7. 并发 Size() 读取
 *
 * 在 TSAN 模式下（cmake -DCABE_ENABLE_TSAN=ON）意义最大；
 * 普通构建也能捕获数据竞争导致的崩溃或错误内容。
 */

#include <gtest/gtest.h>
#include "engine/engine.h"
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <latch>
#include <thread>
#include <unistd.h>
#include <vector>

static bool ThreadTestSupportsDirectIO(const char* path) {
    int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    // 256 MiB：线程测试并发写多 key，需要更大的空间
    if (ftruncate(fd, 256 * 1024 * 1024) < 0) {
        ::close(fd);
        ::unlink(path);
        return false;
    }
    ::close(fd);
    fd = ::open(path, O_RDWR | O_DIRECT | O_SYNC);
    if (fd < 0) {
        ::unlink(path);
        return false;
    }
    ::close(fd);
    return true;
}

class EngineThreadTest : public ::testing::Test {
protected:
    Engine engine_;
    std::string devicePath_;

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        devicePath_ = std::string("/var/tmp/cabe_thread_")
                    + info->test_suite_name() + "_" + info->name()
                    + "_" + std::to_string(::getpid()) + ".dat";

        if (!ThreadTestSupportsDirectIO(devicePath_.c_str())) {
            GTEST_SKIP() << "O_DIRECT not supported at " << devicePath_;
        }
        ASSERT_EQ(SUCCESS, engine_.Open(devicePath_));
    }

    void TearDown() override {
        if (engine_.IsOpen()) {
            engine_.Close();
        }
        ::unlink(devicePath_.c_str());
    }

    static std::vector<char> MakeData(size_t size, char fill) {
        return std::vector<char>(size, fill);
    }
};

// ============================================================
// 1. 多线程并发 Get（shared_lock 允许真并发读）
//
// 先写入固定内容，再用 N 线程同时 Get，
// 所有线程的每一次读取内容必须完整且正确。
// ============================================================
TEST_F(EngineThreadTest, ConcurrentGetSameKey) {
    const size_t valueSize = CABE_VALUE_DATA_SIZE;
    auto data = MakeData(valueSize, 'G');
    ASSERT_EQ(SUCCESS, engine_.Put("cg_key", {data.data(), data.size()}));

    // 6 线程：留 2 个 BufferPool slot 作为裕量（池大小 = 8）
    constexpr int kThreads = 6;
    constexpr int kIters   = 30;
    std::latch start(kThreads);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            std::vector<char> buf(valueSize);
            uint64_t readSize = 0;

            for (int i = 0; i < kIters; ++i) {
                const int32_t rc = engine_.Get("cg_key", {buf.data(), buf.size()}, &readSize);
                if (rc != SUCCESS || readSize != valueSize) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                for (size_t b = 0; b < valueSize; ++b) {
                    if (buf[b] != 'G') {
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
// 2. 并发 Put 不同 key（写互斥，结果全部可见）
//
// N 线程各自写 [key_0 .. key_N-1]，完成后串行验证全部可读。
// ============================================================
TEST_F(EngineThreadTest, ConcurrentPutDifferentKeys) {
    constexpr int kThreads = 6;
    constexpr int kIters   = 5;
    std::latch start(kThreads);
    std::atomic<int> putErrors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            const std::string key = "pk_key_" + std::to_string(t);
            auto data = MakeData(CABE_VALUE_DATA_SIZE, static_cast<char>('A' + t));

            for (int i = 0; i < kIters; ++i) {
                if (engine_.Put(key, {data.data(), data.size()}) != SUCCESS) {
                    putErrors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    ASSERT_EQ(0, putErrors.load());

    // 串行验证：每个 key 的最终内容必须是对应线程的 fill 字节
    for (int t = 0; t < kThreads; ++t) {
        const std::string key  = "pk_key_" + std::to_string(t);
        const char         fill = static_cast<char>('A' + t);
        std::vector<char>  buf(CABE_VALUE_DATA_SIZE);
        uint64_t readSize = 0;
        ASSERT_EQ(SUCCESS, engine_.Get(key, {buf.data(), buf.size()}, &readSize));
        EXPECT_EQ(static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), readSize);
        for (size_t b = 0; b < readSize; ++b) {
            EXPECT_EQ(fill, buf[b]) << "key=" << key << " byte=" << b;
            if (buf[b] != fill) break;
        }
    }
}

// ============================================================
// 3. 并发 Put 同一 key（last-writer-wins，无数据撕裂）
//
// N 线程反复覆盖同一 key，不崩溃。
// 最终读出的内容必须是某一次完整写入的 fill（所有字节相同）。
// ============================================================
TEST_F(EngineThreadTest, ConcurrentPutSameKey) {
    constexpr int kThreads = 4;
    constexpr int kIters   = 15;
    std::latch start(kThreads);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            auto data = MakeData(CABE_VALUE_DATA_SIZE, static_cast<char>('A' + t));

            for (int i = 0; i < kIters; ++i) {
                if (engine_.Put("same_key", {data.data(), data.size()}) != SUCCESS) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(0, errors.load());

    // 最终内容必须是某线程完整写入的 fill（无撕裂）
    std::vector<char> buf(CABE_VALUE_DATA_SIZE);
    uint64_t readSize = 0;
    ASSERT_EQ(SUCCESS, engine_.Get("same_key", {buf.data(), buf.size()}, &readSize));
    EXPECT_EQ(static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), readSize);

    const char first = buf[0];
    EXPECT_TRUE(first >= 'A' && first < 'A' + kThreads)
        << "unexpected fill: " << static_cast<int>(first);
    for (size_t b = 1; b < readSize; ++b) {
        EXPECT_EQ(first, buf[b]) << "torn read at byte " << b;
        if (buf[b] != first) break;
    }
}

// ============================================================
// 4. 并发 Put + Get 混合（读写互斥，读不见中间态）
//
// 1 个 writer 线程持续覆盖 key；N 个 reader 线程持续 Get。
// 每次 Get 要么成功并返回完整一致的内容（所有字节相同），
// 要么返回非 SUCCESS 错误码（不允许发生）。
// ============================================================
TEST_F(EngineThreadTest, ConcurrentPutAndGet) {
    // 写入初始值确保 reader 开始时 key 已存在
    auto initData = MakeData(CABE_VALUE_DATA_SIZE, 'I');
    ASSERT_EQ(SUCCESS, engine_.Put("rw_key", {initData.data(), initData.size()}));

    // 4 reader + 1 writer（共 5 个 BufferPool slot，池有 8，留裕量）
    constexpr int kReaders     = 4;
    constexpr int kWriterIters = 40;
    constexpr int kReaderIters = 60;
    std::latch start(kReaders + 1);
    std::atomic<int> readErrors{0};
    std::vector<std::thread> threads;
    threads.reserve(kReaders + 1);

    // writer
    threads.emplace_back([&] {
        start.arrive_and_wait();
        for (int i = 0; i < kWriterIters; ++i) {
            auto data = MakeData(CABE_VALUE_DATA_SIZE, static_cast<char>('A' + (i % 26)));
            engine_.Put("rw_key", {data.data(), data.size()});
        }
    });

    // readers
    for (int t = 0; t < kReaders; ++t) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            std::vector<char> buf(CABE_VALUE_DATA_SIZE);
            uint64_t readSize = 0;

            for (int i = 0; i < kReaderIters; ++i) {
                const int32_t rc = engine_.Get("rw_key", {buf.data(), buf.size()}, &readSize);
                if (rc != SUCCESS || readSize != CABE_VALUE_DATA_SIZE) {
                    readErrors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                // 所有字节必须相同（来自某次完整的 fill 写入）
                const char first = buf[0];
                for (size_t b = 1; b < readSize; ++b) {
                    if (buf[b] != first) {
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
// 5. 并发完整生命周期（Put → Delete → Remove）
//
// N 线程各自对专属 key 循环执行完整生命周期。
// 最终所有 key 都经过 Remove，Size() 必须为 0。
// ============================================================
TEST_F(EngineThreadTest, ConcurrentLifecyclePerKey) {
    constexpr int kThreads = 6;
    constexpr int kIters   = 8;
    std::latch start(kThreads);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            const std::string key = "lc_key_" + std::to_string(t);
            // 512 KiB：单 chunk，减少每次 I/O 时间以加速测试
            auto data = MakeData(CABE_VALUE_DATA_SIZE / 2, static_cast<char>('A' + t));

            for (int i = 0; i < kIters; ++i) {
                if (engine_.Put(key, {data.data(), data.size()}) != SUCCESS) {
                    errors.fetch_add(1, std::memory_order_relaxed); continue;
                }
                if (engine_.Delete(key) != SUCCESS) {
                    errors.fetch_add(1, std::memory_order_relaxed); continue;
                }
                if (engine_.Remove(key) != SUCCESS) {
                    errors.fetch_add(1, std::memory_order_relaxed); continue;
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(0, errors.load());
    EXPECT_EQ(0u, engine_.Size());
}

// ============================================================
// 6. 并发多 chunk 写入与读取
//
// N 线程各自写入 2 MiB（2 chunk）的 value 并读回校验。
// ============================================================
TEST_F(EngineThreadTest, ConcurrentMultiChunkPutGet) {
    constexpr int kThreads = 4;
    const size_t  valueSize = CABE_VALUE_DATA_SIZE * 2;
    std::latch start(kThreads);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            const std::string key  = "mc_key_" + std::to_string(t);
            const char         fill = static_cast<char>('A' + t);
            auto data = MakeData(valueSize, fill);

            if (engine_.Put(key, {data.data(), data.size()}) != SUCCESS) {
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            std::vector<char> buf(valueSize);
            uint64_t readSize = 0;
            if (engine_.Get(key, {buf.data(), buf.size()}, &readSize) != SUCCESS
                || readSize != valueSize) {
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (size_t b = 0; b < readSize; ++b) {
                if (buf[b] != fill) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(0, errors.load());
    EXPECT_EQ(static_cast<size_t>(kThreads), engine_.Size());
}

// ============================================================
// 7. 并发 Size() 读取（shared_lock 下多线程并发读状态）
//
// 先写入若干 key，再用 N 线程并发调用 Size()。
// 不崩溃，返回值始终在 [0, kKeys] 范围内。
// ============================================================
TEST_F(EngineThreadTest, ConcurrentSizeRead) {
    constexpr int kKeys    = 5;
    constexpr int kThreads = 8;
    constexpr int kIters   = 50;

    for (int i = 0; i < kKeys; ++i) {
        auto data = MakeData(512, static_cast<char>('A' + i));
        const std::string key = "sz_key_" + std::to_string(i);
        ASSERT_EQ(SUCCESS, engine_.Put(key, {data.data(), data.size()}));
    }

    std::latch start(kThreads);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            for (int i = 0; i < kIters; ++i) {
                if (engine_.Size() > static_cast<size_t>(kKeys)) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(0, errors.load());
    EXPECT_EQ(static_cast<size_t>(kKeys), engine_.Size());
}
