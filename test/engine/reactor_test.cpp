// P7M1/M2：reactor 机制测试。全走公开 API、无伪造。M1 验异步机制（投递/事件循环/执行体派发/
// 唤醒/等待 + drain-then-close + 前置校验）；M2 加写路径：Put-Get 命中（首次跑通 ExecuteGet
// 取数路径）+ 多轮 Put→Get→Delete 压测（TSAN 下验写交接 race-free）。
// 设计依据：doc/P7/P7M1_reactor_skeleton_design.md §8、doc/P7/P7M2_write_path_design.md §9
//
// liveness：死锁/丢唤醒表现为 ctest 超时（不是假绿）。TSAN（sync 后端）下跑本组用例验
// caller↔reactor 交接 race-free——单 caller 也是 caller + reactor 两线程。

#include "engine/engine.h"
#include "test/common/test_env.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using cabe::test::GetEnv;

// =========================================================================
// 需要 3 个 loop 设备（数据 + WAL + 快照）：CABE_TEST_DEVICE / _WAL_DEVICE / _SNAPSHOT_DEVICE
// =========================================================================
class ReactorTest : public ::testing::Test {
protected:
    void SetUp() override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_  = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            GTEST_SKIP() << "需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE";
        }
    }
    void TearDown() override { engine_.Close(); }

    cabe::Options CreateOpts() {
        cabe::Options opts;
        opts.devices.push_back({data_, wal_, snap_});
        opts.create = true;
        opts.snapshot_threshold_bytes = 1024 * 1024;   // 同 engine_test：过小 WAL 设备的容量校验
        return opts;
    }

    cabe::Engine engine_;
    std::string data_, wal_, snap_;
};

// —— 机制核心：Get 不存在的 key，走完 reactor 一圈，返 kIndexKeyNotFound（零数据）——
TEST_F(ReactorTest, GetMissThroughReactor) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("no-such", cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
}

// —— 可重复、无丢唤醒（挂了 = ctest 超时）——
TEST_F(ReactorTest, RepeatedGetMiss) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    std::vector<std::byte> out(cabe::kValueSize);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(engine_.Get("k" + std::to_string(i), cabe::DataBuffer{out}).code,
                  cabe::err::kIndexKeyNotFound);
    }
}

// —— drain-then-close：Open → 若干 Get → Close 干净退出 ——
TEST_F(ReactorTest, OpenGetCloseClean) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    EXPECT_TRUE(engine_.is_open());
    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("x", cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
    EXPECT_TRUE(engine_.Close().ok());
    EXPECT_FALSE(engine_.is_open());
}

// —— 析构自动 Close + join（reactor 线程干净退出）——
TEST_F(ReactorTest, DestructorAutoCloses) {
    cabe::Engine tmp;
    ASSERT_TRUE(tmp.Open(CreateOpts()).ok());
    // tmp 析构：opened_ → 自动 Close → 逐 reactor Stop + join
}

// —— Open 两阶段对 recover 模式也成立（空设备；带数据取值是 M2）——
TEST_F(ReactorTest, RecoverEmptyThenGetMiss) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    ASSERT_TRUE(engine_.Close().ok());
    cabe::Options ro;
    ro.devices.push_back({data_, wal_, snap_});
    ro.create = false;
    ro.snapshot_threshold_bytes = 1024 * 1024;
    ASSERT_TRUE(engine_.Open(ro).ok());
    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("absent", cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
}

// —— M2：Put-Get 命中，首次跑通 ExecuteGet 完整取数路径（pool→io.Read→CRC→memcpy）——
TEST_F(ReactorTest, PutGetHitThroughReactor) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    std::vector<std::byte> val(cabe::kValueSize, std::byte{0x42});
    ASSERT_EQ(engine_.Put("k", cabe::DataView{val}).code, cabe::err::kSuccess);
    std::vector<std::byte> out(cabe::kValueSize, std::byte{0});
    ASSERT_EQ(engine_.Get("k", cabe::DataBuffer{out}).code, cabe::err::kSuccess);
    EXPECT_EQ(out, val);
}

// —— M2：写路径压测/活性。多轮 Put→Get(命中)→Delete→Get(miss)，验正确 + 不挂（挂 = ctest 超时）。
//    TSAN（sync 后端）下是写交接 race-free 的最硬证据。——
TEST_F(ReactorTest, WritePathStress) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    std::vector<std::byte> out(cabe::kValueSize);
    for (int i = 0; i < 300; ++i) {
        const std::string key = "wk" + std::to_string(i);
        std::vector<std::byte> val(cabe::kValueSize, static_cast<std::byte>(i & 0xFF));
        ASSERT_EQ(engine_.Put(key, cabe::DataView{val}).code, cabe::err::kSuccess);
        ASSERT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kSuccess);
        EXPECT_EQ(out, val);
        ASSERT_EQ(engine_.Delete(key).code, cabe::err::kSuccess);
        EXPECT_EQ(engine_.Get(key, cabe::DataBuffer{out}).code, cabe::err::kIndexKeyNotFound);
    }
}

// —— Get 前置校验（开着引擎）——
TEST_F(ReactorTest, GetEmptyKeyAndWrongSize) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(engine_.Get("", cabe::DataBuffer{out}).code, cabe::err::kMemEmptyKey);
    std::vector<std::byte> small(100);
    EXPECT_EQ(engine_.Get("k", cabe::DataBuffer{small}).code, cabe::err::kEngineInvalidValue);
}

} // namespace

// =========================================================================
// 不需设备：未开 + 坏 Open
// =========================================================================
TEST(Reactor, GetWithoutOpenFails) {
    cabe::Engine e;
    std::vector<std::byte> out(cabe::kValueSize);
    EXPECT_EQ(e.Get("k", cabe::DataBuffer{out}).code, cabe::err::kEngineNotOpen);
}

// —— not-open 守卫补全：Delete / SetWalLevel / Snapshot 与 Get 同样在未开时即拒（kEngineNotOpen）——
TEST(Reactor, DeleteWithoutOpenFails) {
    cabe::Engine e;
    EXPECT_EQ(e.Delete("k").code, cabe::err::kEngineNotOpen);
}

TEST(Reactor, SetWalLevelWithoutOpenFails) {
    cabe::Engine e;
    EXPECT_EQ(e.SetWalLevel(cabe::WalLevel::WalSync).code, cabe::err::kEngineNotOpen);
}

TEST(Reactor, SnapshotWithoutOpenFails) {
    cabe::Engine e;
    EXPECT_EQ(e.Snapshot().code, cabe::err::kEngineNotOpen);
}

// 注：下两条 EmptyDevicesFails / TooManyDevicesFails 与 engine_test.cpp 有逐字相同的 Engine-suite
// 版本；此为 Reactor-suite 的对称覆盖（同款负向 Open 校验、不同 suite），刻意各留一份，非漏改。
TEST(Reactor, EmptyDevicesFails) {
    cabe::Engine e;
    EXPECT_EQ(e.Open(cabe::Options{}).code, cabe::err::kEngineInvalidOpts);
}

// P7M4：N>1 现合法（多设备）；负向测改为守 N≤256 上界（DeviceId=uint8_t）。
TEST(Reactor, TooManyDevicesFails) {
    cabe::Engine e;
    cabe::Options opts;
    for (int i = 0; i < 257; ++i) opts.devices.push_back({"d", "w", "s"});   // size 校验在开设备前，不碰假路径
    EXPECT_EQ(e.Open(opts).code, cabe::err::kEngineInvalidOpts);
}
