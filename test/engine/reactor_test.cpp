// P7M1：reactor 机制测试。全走公开 API、无伪造、无 Put——只验异步机制（投递/事件循环/
// 执行体派发/唤醒/等待 + drain-then-close + 前置校验 + not-implemented）。取值正确性与
// recover 带数据后 Get 随 M2 的 Put-Get 联动测。设计依据：doc/P7/P7M1_reactor_skeleton_design.md §8
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

// —— M1 范围：写类公开方法返 not-implemented ——
TEST_F(ReactorTest, WriteOpsNotImplemented) {
    ASSERT_TRUE(engine_.Open(CreateOpts()).ok());
    std::vector<std::byte> val(cabe::kValueSize);
    EXPECT_EQ(engine_.Put("k", cabe::DataView{val}).code, cabe::err::kEngineNotImplemented);
    EXPECT_EQ(engine_.Delete("k").code, cabe::err::kEngineNotImplemented);
    EXPECT_EQ(engine_.SetWalLevel(cabe::WalLevel::Strict).code, cabe::err::kEngineNotImplemented);
    EXPECT_EQ(engine_.Snapshot().code, cabe::err::kEngineNotImplemented);
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

TEST(Reactor, EmptyDevicesFails) {
    cabe::Engine e;
    EXPECT_EQ(e.Open(cabe::Options{}).code, cabe::err::kEngineInvalidOpts);
}

TEST(Reactor, MultipleDevicesFails) {
    cabe::Engine e;
    cabe::Options opts;
    opts.devices.push_back({"d1", "w1", "s1"});
    opts.devices.push_back({"d2", "w2", "s2"});
    EXPECT_EQ(e.Open(opts).code, cabe::err::kEngineInvalidOpts);
}
