// P7M3：单设备多线程并发测试。多个调用线程并发经 Engine 公开 API 砸单 reactor（N=1）。
// reactor 机制从 M1 起就是 MPSC（CAS-push、per-caller wake、per-op result、单消费者串行）——
// 本组用例不验机制实现，而是用并发实测 + TSAN 多轮把它钉死。活性：死锁/丢唤醒 = ctest 超时（非误绿）。
// 契约（P7M3-D2）：Open/Close 排他——每个用例"先 join 全部工作线程，再 Close"（TearDown），示范静默契约。
// 设计依据：doc/P7/P7M3_single_device_mt_design.md §7
//
// 需要 3 个 loop 设备：CABE_TEST_DEVICE / _WAL_DEVICE / _SNAPSHOT_DEVICE
// io_uring 后端用 ASAN/UBSAN（io_uring 与 TSAN 不兼容）；race 检查归 sync 后端（交接逻辑后端无关）。

#include "engine/engine.h"
#include "test/common/test_env.h"
#include "util/raw_device.h"   // recover 用例清盘（消共用设备的前序残留）

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using cabe::test::GetEnv;

// 线程数：≥2 才有争用，封顶 8 免 TSAN 太慢。
std::size_t Threads() {
    std::size_t n = std::thread::hardware_concurrency();
    if (n < 2) n = 2;
    if (n > 8) n = 8;
    return n;
}

// 1 MiB 全 b 填充值（可辨认模式：同 key 用例据此判"读到谁的值 + 是否撕裂"）。
std::vector<std::byte> MakeValue(std::byte b) {
    return std::vector<std::byte>(cabe::kValueSize, b);
}

// 起 n 个线程各跑 body(tid)，全部 join 后返回。死锁/丢唤醒 = 挂在 join → ctest 超时兜底。
void RunThreads(std::size_t n, const std::function<void(std::size_t)>& body) {
    std::vector<std::thread> ts;
    ts.reserve(n);
    for (std::size_t tid = 0; tid < n; ++tid) ts.emplace_back([&body, tid] { body(tid); });
    for (auto& t : ts) t.join();
}

// 需要 3 个 loop 设备（数据 + WAL + 快照）。
class ConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_  = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            GTEST_SKIP() << "需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE";
        }
    }
    void TearDown() override { engine_.Close(); }   // 契约：此刻工作线程已全 join（静默），Close 安全

    cabe::Options CreateOpts() {
        cabe::Options opts;
        opts.devices.push_back({data_, wal_, snap_});
        opts.create = true;
        opts.snapshot_threshold_bytes = 1024 * 1024;
        return opts;
    }

    // 抹 WAL 环区 + 快照双槽头，清除前序用例在共用 loop 设备上残留的陈帧/陈快照。
    // create 只重置环元数据、不抹盘数据区 → recover 会撞陈帧（越容差活帧）；范式同
    // test_wal_concurrency 的"先清环"、recovery_test 的抹槽头。数据盘陈块无需抹（create 重置分配器，
    // recover 重建索引只引用本会话新写的块）。
    void WipeWalAndSnapshot() const {
        {   // WAL：抹数据区全部（kDataRegionOffset..end），清陈帧。
            cabe::RawDevice dev;
            ASSERT_EQ(dev.Open(wal_), cabe::err::kSuccess);
            const std::uint64_t size = dev.SizeBytes();
            const std::size_t chunk = std::size_t{1} << 20;   // 1 MiB（4K 对齐；环区起止皆 4K 对齐）
            std::byte* z = cabe::RawDevice::AllocAligned(chunk);
            ASSERT_NE(z, nullptr);
            std::memset(z, 0, chunk);
            for (std::uint64_t off = cabe::kDataRegionOffset; off < size; off += chunk) {
                const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, size - off));
                ASSERT_EQ(dev.WriteAt(off, z, n), cabe::err::kSuccess);
            }
            cabe::RawDevice::FreeAligned(z);
            dev.Close();
        }
        {   // 快照：抹双槽头，雕"从未快照"。
            cabe::RawDevice dev;
            ASSERT_EQ(dev.Open(snap_), cabe::err::kSuccess);
            const std::uint64_t slot = cabe::SnapshotSlotSize(dev.SizeBytes());
            ASSERT_GT(slot, 0u);
            std::byte* z = cabe::RawDevice::AllocAligned(cabe::kSnapshotSlotHeaderSize);
            ASSERT_NE(z, nullptr);
            std::memset(z, 0, cabe::kSnapshotSlotHeaderSize);
            ASSERT_EQ(dev.WriteAt(cabe::kDataRegionOffset, z, cabe::kSnapshotSlotHeaderSize), cabe::err::kSuccess);
            ASSERT_EQ(dev.WriteAt(cabe::kDataRegionOffset + slot, z, cabe::kSnapshotSlotHeaderSize), cabe::err::kSuccess);
            cabe::RawDevice::FreeAligned(z);
            dev.Close();
        }
    }

    cabe::Engine engine_;
    std::string data_, wal_, snap_;
};

// ① 纯交接压测：N 线程狂跑 Get-miss（无 I/O），最大化 CAS 入栈 + 唤醒边覆盖。
TEST_F(ConcurrencyTest, ConcurrentGetMissStorm) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    const std::size_t N = Threads();
    std::barrier gate(static_cast<std::ptrdiff_t>(N));
    RunThreads(N, [&](std::size_t tid) {
        std::vector<std::byte> out(cabe::kValueSize);
        for (std::size_t i = 0; i < 1000; ++i) {
            if (i < 200) gate.arrive_and_wait();   // 前 200 轮 barrier 对齐（同刻砸 inbox），后 800 轮自由跑
            const std::string key = "miss_" + std::to_string(tid) + "_" + std::to_string(i);
            EXPECT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
        }
    });
}

// ①③ 不相交 key：各线程独占 key 空间，Put→Get（读己之写）→Delete→Get（miss）。Put/Delete 循环回收块。
TEST_F(ConcurrencyTest, ConcurrentDisjointRoundTrip) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    const std::size_t N = Threads();
    RunThreads(N, [&](std::size_t tid) {
        std::vector<std::byte> out(cabe::kValueSize);
        for (std::size_t i = 0; i < 80; ++i) {
            const std::string key = "t" + std::to_string(tid) + "_k" + std::to_string(i);
            const auto val = MakeValue(static_cast<std::byte>((tid * 31 + i) & 0xFF));
            EXPECT_EQ(engine_.Put(key, cabe::DataView{val}).code, cabe::err::kSuccess);
            EXPECT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kSuccess);
            EXPECT_EQ(out, val);                                          // 独占 key：读己之写成立
            EXPECT_EQ(engine_.Delete(key).code, cabe::err::kSuccess);
            EXPECT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
        }
    });
}

// ② SetWalLevel‖并发 Put：1 线程轮换四档，N-1 线程写。运营 op 与数据 op 经 reactor 串行、无锁安全。
TEST_F(ConcurrencyTest, SetWalLevelDuringPut) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    const std::size_t N = Threads();
    std::atomic<bool> stop{false};
    std::thread switcher([&] {
        const cabe::WalLevel levels[] = {cabe::WalLevel::Strict, cabe::WalLevel::ValueSync,
                                         cabe::WalLevel::WalSync, cabe::WalLevel::Async};
        std::size_t k = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            EXPECT_EQ(engine_.SetWalLevel(levels[k++ % 4]).code, cabe::err::kSuccess);
        }
    });
    const std::size_t writers = N > 1 ? N - 1 : 1;
    RunThreads(writers, [&](std::size_t tid) {
        std::vector<std::byte> out(cabe::kValueSize);
        for (std::size_t i = 0; i < 80; ++i) {
            const std::string key = "sw" + std::to_string(tid) + "_" + std::to_string(i);
            const auto val = MakeValue(static_cast<std::byte>((tid + i) & 0xFF));
            EXPECT_EQ(engine_.Put(key, cabe::DataView{val}).code, cabe::err::kSuccess);
            EXPECT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kSuccess);
            EXPECT_EQ(out, val);                          // 独占 key + 切档不影响进程内读回
            EXPECT_EQ(engine_.Delete(key).code, cabe::err::kSuccess);
        }
    });
    stop.store(true, std::memory_order_relaxed);
    switcher.join();                                       // 先 join 运营线程，再（TearDown）Close
}

// ② Snapshot‖并发 Put：1 线程狂触发手动快照 + 小阈值让自动快照也插进来，N-1 线程写。
TEST_F(ConcurrencyTest, SnapshotDuringPut) {
    auto opts = CreateOpts();
    opts.snapshot_threshold_bytes = 64 * 1024;            // 小阈值：自动快照频繁插入（16MiB WAL 盘容此环）
    ASSERT_TRUE(engine_.Open(opts).ok());
    const std::size_t N = Threads();
    std::atomic<bool> stop{false};
    std::thread snapper([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            EXPECT_EQ(engine_.Snapshot().code, cabe::err::kSuccess);
        }
    });
    const std::size_t writers = N > 1 ? N - 1 : 1;
    RunThreads(writers, [&](std::size_t tid) {
        std::vector<std::byte> out(cabe::kValueSize);
        for (std::size_t i = 0; i < 80; ++i) {
            const std::string key = "sn" + std::to_string(tid) + "_" + std::to_string(i);
            const auto val = MakeValue(static_cast<std::byte>((tid * 7 + i) & 0xFF));
            EXPECT_EQ(engine_.Put(key, cabe::DataView{val}).code, cabe::err::kSuccess);
            EXPECT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kSuccess);
            EXPECT_EQ(out, val);
            EXPECT_EQ(engine_.Delete(key).code, cabe::err::kSuccess);
        }
    });
    stop.store(true, std::memory_order_relaxed);
    snapper.join();
}

// ① 同 key 并发：N 线程砸同一 key（各写全 byte{tid}），验无撕裂 + 自洽（读到某线程的完整值，CRC 对）。
TEST_F(ConcurrencyTest, ConcurrentSameKeyNoTear) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    const std::size_t N = Threads();
    const std::string key = "shared";
    RunThreads(N, [&](std::size_t tid) {
        const auto val = MakeValue(static_cast<std::byte>(tid));
        std::vector<std::byte> out(cabe::kValueSize);
        for (std::size_t i = 0; i < 100; ++i) {
            EXPECT_EQ(engine_.Put(key, cabe::DataView{val}).code, cabe::err::kSuccess);
            const auto st = engine_.Get(key, cabe::DataBuffer{out});
            EXPECT_EQ(st.code, cabe::err::kSuccess);
            if (st.code != cabe::err::kSuccess) continue;
            const std::byte b = out[0];
            // 无撕裂：整块均匀字节（撕裂会读到两个线程值的拼接）。
            EXPECT_TRUE(std::all_of(out.begin(), out.end(), [b](std::byte x) { return x == b; }))
                << "撕裂：同 key 值非均匀字节";
            // 自洽：读到的字节是某个合法 tid（到达序最后写赢，不可预测具体是谁——故不断言特定值）。
            EXPECT_LT(std::to_integer<unsigned>(b), static_cast<unsigned>(N))
                << "读到非任何线程写入的值";
        }
    });
}

// ③ 并发写 → 静默 → Close → 重开 recover，终态精确一致。终态在生量控小以适配 64MiB 数据盘。
TEST_F(ConcurrencyTest, ConcurrentWritesRecoverConsistent) {
    const std::size_t N = Threads();
    const std::size_t per = std::max<std::size_t>(2, 24 / N);   // 终态 ≈24 个值（≈24MiB ≪ 64MiB 数据盘）
    auto expected = [](std::size_t tid, std::size_t i) {
        return MakeValue(static_cast<std::byte>((tid * 13 + i) & 0xFF));
    };
    // 共用 loop 设备：先抹 WAL 环区 + 快照槽头，清除前序用例残留（recover 对设备洁净敏感）。
    WipeWalAndSnapshot();
    // 会话一：并发写到已知终态。
    {
        cabe::Engine eng;
        ASSERT_TRUE(eng.Open(CreateOpts()).ok());
        RunThreads(N, [&](std::size_t tid) {
            for (std::size_t i = 0; i < per; ++i) {
                const std::string key = "rec" + std::to_string(tid) + "_" + std::to_string(i);
                EXPECT_EQ(eng.Put(key, cabe::DataView{expected(tid, i)}).code, cabe::err::kSuccess);
            }
        });
        ASSERT_TRUE(eng.Close().ok());   // RunThreads 已 join → 静默 → Close 安全（契约）
    }
    // 会话二：recover 校验终态。
    cabe::Engine eng2;
    cabe::Options ro = CreateOpts();
    ro.create = false;
    ASSERT_TRUE(eng2.Open(ro).ok());
    std::vector<std::byte> out(cabe::kValueSize);
    for (std::size_t tid = 0; tid < N; ++tid) {
        for (std::size_t i = 0; i < per; ++i) {
            const std::string key = "rec" + std::to_string(tid) + "_" + std::to_string(i);
            ASSERT_EQ(eng2.Get(key, cabe::DataBuffer{out}).code, cabe::err::kSuccess) << key;
            EXPECT_EQ(out, expected(tid, i)) << key;
        }
    }
    EXPECT_TRUE(eng2.Close().ok());
}

} // namespace
