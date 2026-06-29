// P7M4：多设备测试。N=2、每设备一个 reactor、按 key 的 hash 路由(RouteToDevice)。
// 经 Engine 公开 API 验:key 跨设备分散 + 同 key 恒同设备(①);两设备各自 recover 一致(②);
// 运营口 Snapshot/SetWalLevel/Close 触达全部 reactor(③);dev() 无错位——靠 recover 拒 dev 错位的块 +
// ExecuteGet 的 debug 断言双兜(④);多线程×多设备复合 race-free(⑤,M3 机制 × 独立 reactor)。
// 设计依据:doc/P7/P7M4_multi_device_design.md §7
//
// 需要**两组**共 6 块 loop 设备:组 1 = CABE_TEST_DEVICE/_WAL_DEVICE/_SNAPSHOT_DEVICE;
// 组 2 = CABE_TEST_DEVICE2/_WAL_DEVICE2/_SNAPSHOT_DEVICE2(`scripts/mkloop.sh create-multi` 一键建)。
// 缺组 2 则 GTEST_SKIP。io_uring 后端用 ASAN/UBSAN;race 检查归 sync。

#include "engine/engine.h"
#include "test/common/test_env.h"
#include "util/hash.h"          // RouteToDevice(观察 key 分布)
#include "util/raw_device.h"    // recover 用例清盘

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using cabe::test::GetEnv;

std::size_t Threads() {
    std::size_t n = std::thread::hardware_concurrency();
    if (n < 2) n = 2;
    if (n > 8) n = 8;
    return n;
}

std::vector<std::byte> MakeValue(std::byte b) {
    return std::vector<std::byte>(cabe::kValueSize, b);
}

void RunThreads(std::size_t n, const std::function<void(std::size_t)>& body) {
    std::vector<std::thread> ts;
    ts.reserve(n);
    for (std::size_t tid = 0; tid < n; ++tid) ts.emplace_back([&body, tid] { body(tid); });
    for (auto& t : ts) t.join();   // 活性:死锁/丢唤醒 = 挂这里 → ctest 超时
}

// 需要两组 loop 设备。
class MultiDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        d1_ = GetEnv("CABE_TEST_DEVICE");   w1_ = GetEnv("CABE_TEST_WAL_DEVICE");   s1_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        d2_ = GetEnv("CABE_TEST_DEVICE2");  w2_ = GetEnv("CABE_TEST_WAL_DEVICE2");  s2_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE2");
        if (d1_.empty() || w1_.empty() || s1_.empty() || d2_.empty() || w2_.empty() || s2_.empty()) {
            GTEST_SKIP() << "需要两组 loop 设备(CABE_TEST_*  +  CABE_TEST_*2;见 mkloop.sh create-multi)";
        }
    }
    void TearDown() override { engine_.Close(); }   // 契约:工作线程已 join(静默),Close 安全

    cabe::Options MakeOpts(bool create) const {
        cabe::Options o;
        o.create = create;
        o.snapshot_threshold_bytes = 1024 * 1024;
        o.devices.push_back({d1_, w1_, s1_});
        o.devices.push_back({d2_, w2_, s2_});   // N=2
        return o;
    }

    // 抹一组的 WAL 环区 + 快照双槽头(同 M3 清盘范式;recover 用例对设备洁净敏感)。
    void WipeGroup(const std::string& wal, const std::string& snap) const {
        {   // WAL:抹数据区全部,清陈帧。
            cabe::RawDevice dev;
            ASSERT_EQ(dev.Open(wal), cabe::err::kSuccess);
            const std::uint64_t size = dev.SizeBytes();
            const std::size_t chunk = std::size_t{1} << 20;
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
        {   // 快照:抹双槽头,雕"从未快照"。
            cabe::RawDevice dev;
            ASSERT_EQ(dev.Open(snap), cabe::err::kSuccess);
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
    void WipeBoth() const { WipeGroup(w1_, s1_); WipeGroup(w2_, s2_); }

    cabe::Engine engine_;
    std::string d1_, w1_, s1_, d2_, w2_, s2_;
};

// ① 跨设备分散 + 同 key 恒同设备:用公开 RouteToDevice 确认 key 集跨两设备,再端到端往返。
TEST_F(MultiDeviceTest, DistributedRoundTrip) {
    ASSERT_TRUE(engine_.Open(MakeOpts(true)).ok());
    std::set<cabe::DeviceId> hit;
    std::vector<std::string> keys;
    for (int i = 0; i < 40; ++i) {
        keys.push_back("mk" + std::to_string(i));
        hit.insert(cabe::util::RouteToDevice(keys.back(), 2));
    }
    ASSERT_EQ(hit.size(), 2u) << "测试 key 集未跨两设备(hash 分布异常)";

    std::vector<std::byte> out(cabe::kValueSize);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto val = MakeValue(static_cast<std::byte>(i & 0xFF));
        EXPECT_EQ(engine_.Put(keys[i], cabe::DataView{val}).code, cabe::err::kSuccess);
        EXPECT_EQ(engine_.Get(keys[i], cabe::DataBuffer{out}).code, cabe::err::kSuccess);
        EXPECT_EQ(out, val);                                              // 同 key 两次路由同设备 → 读回
        EXPECT_EQ(engine_.Delete(keys[i]).code, cabe::err::kSuccess);     // Put→Delete 回收控容量
        EXPECT_EQ(engine_.Get(keys[i], cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
    }
}

// ②④ 两设备各自 recover 一致(含跨重启 Put/Delete/Snapshot)。recover 校验 dev()==device_id ⟹ 证 ④。
TEST_F(MultiDeviceTest, PerDeviceRecover) {
    WipeBoth();   // 清两组陈帧/陈快照(create 不抹盘数据区)
    auto expected = [](int i) { return MakeValue(static_cast<std::byte>((i * 13) & 0xFF)); };
    const int K = 24;   // 终态 ≤24 个值,跨两设备 ≈12/盘 ≪ 60
    {
        cabe::Engine eng;
        ASSERT_TRUE(eng.Open(MakeOpts(true)).ok());
        for (int i = 0; i < K; ++i)
            ASSERT_EQ(eng.Put("rk" + std::to_string(i), cabe::DataView{expected(i)}).code, cabe::err::kSuccess);
        for (int i = 0; i < K; i += 2)   // 删偶数,留奇数为终态
            ASSERT_EQ(eng.Delete("rk" + std::to_string(i)).code, cabe::err::kSuccess);
        ASSERT_EQ(eng.Snapshot().code, cabe::err::kSuccess);   // 跨重启含 Snapshot(广播两设备)
        ASSERT_TRUE(eng.Close().ok());
    }
    cabe::Engine eng2;
    ASSERT_TRUE(eng2.Open(MakeOpts(false)).ok());              // recover N=2
    std::vector<std::byte> out(cabe::kValueSize);
    for (int i = 0; i < K; ++i) {
        const std::string key = "rk" + std::to_string(i);
        if (i % 2 == 1) {
            ASSERT_EQ(eng2.Get(key, cabe::DataBuffer{out}).code, cabe::err::kSuccess) << key;
            EXPECT_EQ(out, expected(i)) << key;
        } else {
            EXPECT_EQ(eng2.Get(key, cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound) << key;
        }
    }
    EXPECT_TRUE(eng2.Close().ok());
}

// ③ 运营口 fan-out:Snapshot/SetWalLevel 触达两 reactor + 数据一致;Close 触达全部靠"干净退出不挂"。
TEST_F(MultiDeviceTest, OperationalFanOut) {
    ASSERT_TRUE(engine_.Open(MakeOpts(true)).ok());
    std::vector<std::byte> out(cabe::kValueSize);
    auto put_check = [&](int base) {
        for (int i = 0; i < 16; ++i) {
            const std::string key = "of" + std::to_string(base) + "_" + std::to_string(i);
            const auto val = MakeValue(static_cast<std::byte>((base + i) & 0xFF));
            ASSERT_EQ(engine_.Put(key, cabe::DataView{val}).code, cabe::err::kSuccess);
            ASSERT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kSuccess);
            EXPECT_EQ(out, val);
            ASSERT_EQ(engine_.Delete(key).code, cabe::err::kSuccess);   // 回收控容量
        }
    };
    put_check(0);
    EXPECT_EQ(engine_.Snapshot().code, cabe::err::kSuccess);            // 广播到两 reactor
    put_check(100);
    for (auto lvl : {cabe::WalLevel::Strict, cabe::WalLevel::Async, cabe::WalLevel::WalSync})
        EXPECT_EQ(engine_.SetWalLevel(lvl).code, cabe::err::kSuccess);  // 广播到两 reactor
    put_check(200);
    EXPECT_EQ(engine_.Snapshot().code, cabe::err::kSuccess);
    EXPECT_TRUE(engine_.Close().ok());   // Close 广播 Stop+join 两 reactor;干净退出 = 触达全部
}

// ⑤ + 复合:N 线程并发跨两设备 Put/Get/Delete。sync+TSAN race-free(M3 机制 × 独立 reactor 的推论)。
//   QPS 随设备扩展只观察(loop 盘不可信,真盘度量留 P11),此处只验正确 + 不挂。
TEST_F(MultiDeviceTest, MultiThreadMultiDevice) {
    ASSERT_TRUE(engine_.Open(MakeOpts(true)).ok());
    const std::size_t N = Threads();
    RunThreads(N, [&](std::size_t tid) {
        std::vector<std::byte> out(cabe::kValueSize);
        for (int i = 0; i < 60; ++i) {
            const std::string key = "mt" + std::to_string(tid) + "_" + std::to_string(i);
            const auto val = MakeValue(static_cast<std::byte>((tid * 17 + i) & 0xFF));
            EXPECT_EQ(engine_.Put(key, cabe::DataView{val}).code, cabe::err::kSuccess);
            EXPECT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kSuccess);
            EXPECT_EQ(out, val);
            EXPECT_EQ(engine_.Delete(key).code, cabe::err::kSuccess);
        }
    });
    // 先 join(RunThreads 内)再 Close(TearDown):M3 静默契约
}

} // namespace
