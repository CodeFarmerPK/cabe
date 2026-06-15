// P6M1：WAL 提交组多线程 harness（设计稿 doc/P6/P6M1_commit_group_design.md §11）。
// 断言手段 = key 编码（"t<线程>-i<序号>"）+ 恢复重放对账——盘面自己作证，不开观测后门。
// 编排两种：屏障组（std::barrier 每轮对齐提交，强制组队）+ 自由跑（全速随机交错）。
// 活性断言（用例 G）：全部线程正常 join；任何死锁/丢失唤醒表现为 ctest 超时而非误绿。
// 注意：harness 直打 Wal、无 Engine 无快照无回收——总帧数必须远小于环容量（撞墙用例
// 反向操作，故意填满）；每个场景先 ZeroWalRing 清环，避免上一场景残帧干扰恢复扫描。

#include "wal/wal.h"
#include "common/error_code.h"
#include "util/raw_device.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

    std::string GetEnv(const char* name) {
        const char* v = std::getenv(name);
        return v == nullptr ? std::string{} : std::string{v};
    }

    // key 编码：线程号 + 序号 → 重放侧能精确回答"谁的哪一帧、以什么 seq、出现几次"。
    std::string MakeKey(std::size_t tid, std::size_t i) {
        char buf[64];   // 两个 size_t 最宽 20 位,-Wformat-truncation 按理论上界算
        std::snprintf(buf, sizeof buf, "t%02zu-i%06zu", tid, i);
        return buf;
    }

    std::vector<std::string> MakeKeys(std::size_t n_threads, std::size_t per_thread) {
        std::vector<std::string> keys(n_threads * per_thread);
        for (std::size_t t = 0; t < n_threads; ++t)
            for (std::size_t i = 0; i < per_thread; ++i)
                keys[t * per_thread + i] = MakeKey(t, i);
        return keys;
    }

    struct Replayed {
        std::uint64_t seq;
        std::string   key;
    };

    constexpr int32_t kNotSubmitted = 999;   // 测试侧哨兵：该下标的调用从未发起

} // namespace

class WalConcurrencyTest : public ::testing::Test {
protected:
    void SetUp() override {
        wal_ = GetEnv("CABE_TEST_WAL_DEVICE");
        if (wal_.empty()) {
            GTEST_SKIP() << "需要 CABE_TEST_WAL_DEVICE";
        }
    }

    // 套件级清洁约定：本目标的用例会在环上留下大量帧（自由跑数千、撞墙用例填满全环），
    // 而后续二进制（snapshot/recovery）的夹具不清环——它们一直依赖"前序用例残留少且
    // seq 小"的隐性顺序约定。离场时把环清零，把约定恢复成显式的"跑完留下干净环"。
    void TearDown() override {
        if (!wal_.empty()) ZeroWalRing();
    }

    // 直驱 Wal（与 wal_test 同款）：create 写侧夹具 / recover 恢复夹具；小阈值过容量校验。
    cabe::Options MakeWalOpts(cabe::WalLevel lvl) const {
        cabe::Options opts;
        opts.create    = true;
        opts.wal_level = lvl;
        opts.snapshot_threshold_bytes = 1024 * 1024;
        return opts;
    }
    cabe::Options MakeWalRecoverOpts(cabe::WalLevel lvl = cabe::WalLevel::WalSync) const {
        cabe::Options opts;
        opts.create    = false;
        opts.wal_level = lvl;
        opts.snapshot_threshold_bytes = 1024 * 1024;
        return opts;
    }

    std::uint64_t RingBytes() const {
        cabe::RawDevice dev;
        EXPECT_EQ(dev.Open(wal_), cabe::err::kSuccess);
        const std::uint64_t r = cabe::WalRingSize(dev.SizeBytes());
        dev.Close();
        return r;
    }

    // 清环：环区全写零（场景隔离——create 只重置内存态，盘上旧帧会干扰下一场景的恢复扫描）。
    void ZeroWalRing() {
        cabe::RawDevice dev;
        ASSERT_EQ(dev.Open(wal_), cabe::err::kSuccess);
        const std::uint64_t end = cabe::kDataRegionOffset + cabe::WalRingSize(dev.SizeBytes());
        constexpr std::size_t kChunk = 1u << 20;
        std::byte* buf = cabe::RawDevice::AllocAligned(kChunk);
        ASSERT_NE(buf, nullptr);
        std::memset(buf, 0, kChunk);
        for (std::uint64_t off = cabe::kDataRegionOffset; off < end; ) {
            const std::size_t len =
                static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, end - off));
            ASSERT_EQ(dev.WriteAt(off, buf, len), cabe::err::kSuccess);
            off += len;
        }
        ASSERT_EQ(dev.Sync(), cabe::err::kSuccess);
        cabe::RawDevice::FreeAligned(buf);
        dev.Close();
    }

    // N 写者 × 每人 per_thread 帧；返回逐调用结果码（下标 = tid*per_thread + i）。
    // 屏障模式每轮全员对齐后同时提交（强制组队）；自由模式全速。线程内不做 gtest 断言
    //（线程安全口径），结果回主线程统一判。
    static std::vector<int32_t> RunWriters(cabe::Wal& wal, const std::vector<std::string>& keys,
                                           std::size_t n_threads, std::size_t per_thread,
                                           bool barrier_mode) {
        std::vector<int32_t> rcs(n_threads * per_thread, kNotSubmitted);
        std::barrier<> gate(static_cast<std::ptrdiff_t>(n_threads));
        std::vector<std::thread> ts;
        ts.reserve(n_threads);
        for (std::size_t tid = 0; tid < n_threads; ++tid) {
            ts.emplace_back([&, tid] {
                for (std::size_t i = 0; i < per_thread; ++i) {
                    if (barrier_mode) gate.arrive_and_wait();
                    const std::size_t idx = tid * per_thread + i;
                    cabe::WalEntry e{};
                    e.type      = cabe::WalEntryType::Put;
                    e.key       = keys[idx];   // 视图指向预生成 vector，活过整个 WriteWal
                    e.block     = cabe::BlockId::Make(0, idx & 0xFFFF);
                    e.value_crc = 1;
                    e.timestamp = 1;
                    rcs[idx] = wal.WriteWal(e);
                }
            });
        }
        for (auto& t : ts) t.join();   // 活性（用例 G）：死锁/丢失唤醒 = 挂在这,ctest 超时兜底
        return rcs;
    }

    // 恢复重放收集（创世重放 covered=0）：盘上每一帧的 (seq, key) 如实交出。
    std::vector<Replayed> RecoverCollect() {
        cabe::Wal wal;
        cabe::Options opts = MakeWalRecoverOpts();
        EXPECT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);
        std::vector<Replayed> out;
        EXPECT_EQ(wal.Recover(0, [&](const cabe::WalEntry& e, std::uint64_t seq) {
            out.push_back({seq, std::string(e.key)});   // 视图仅回调期间有效,即拷
            return cabe::err::kSuccess;
        }), cabe::err::kSuccess);
        wal.Close();
        return out;
    }

    // seq 稠密：无重、自 1 起、至 total 止（I2"恰好一次"与不变量③④的盘上验收）。
    static void VerifySeqDense(const std::vector<Replayed>& got) {
        std::set<std::uint64_t> seqs;
        for (const auto& r : got) seqs.insert(r.seq);
        ASSERT_EQ(seqs.size(), got.size()) << "seq 有重复";
        if (!got.empty()) {
            EXPECT_EQ(*seqs.begin(), 1u) << "seq 不从 1 起";
            EXPECT_EQ(*seqs.rbegin(), got.size()) << "seq 有空洞";
        }
    }

    // key → seq 映射，顺带断言每个 key 恰好出现一次。
    static std::map<std::string, std::uint64_t> KeySeqMap(const std::vector<Replayed>& got) {
        std::map<std::string, std::uint64_t> m;
        for (const auto& r : got) {
            EXPECT_EQ(m.count(r.key), 0u) << "key 重复出现: " << r.key;
            m[r.key] = r.seq;
        }
        return m;
    }

    // 每线程顺序：同一线程第 i 帧的 seq < 第 i+1 帧（阻塞语义给的跨批次顺序，3.2 的盘上印证）。
    static void VerifyPerThreadOrder(const std::map<std::string, std::uint64_t>& m,
                                     std::size_t n_threads, std::size_t per_thread) {
        for (std::size_t t = 0; t < n_threads; ++t) {
            for (std::size_t i = 1; i < per_thread; ++i) {
                const auto prev = m.find(MakeKey(t, i - 1));
                const auto cur  = m.find(MakeKey(t, i));
                ASSERT_NE(prev, m.end());
                ASSERT_NE(cur, m.end());
                EXPECT_LT(prev->second, cur->second)
                    << "线程 " << t << " 第 " << i << " 帧 seq 倒挂";
            }
        }
    }

    std::string wal_;
};

// ---------------- 用例 B：屏障组，N ∈ {2,4,8}，全成功 + 稠密 + 恰好一次 + 每线程序 ----------------
TEST_F(WalConcurrencyTest, BarrierGroupsDense) {
    for (const std::size_t n_threads : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        SCOPED_TRACE("n_threads=" + std::to_string(n_threads));
        ZeroWalRing();
        cabe::Options opts = MakeWalOpts(cabe::WalLevel::WalSync);
        cabe::Wal wal;
        ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

        const std::size_t per = 200;
        const auto keys = MakeKeys(n_threads, per);
        const auto rcs  = RunWriters(wal, keys, n_threads, per, /*barrier=*/true);
        for (std::size_t i = 0; i < rcs.size(); ++i) {
            ASSERT_EQ(rcs[i], cabe::err::kSuccess) << "第 " << i << " 个调用";
        }
        EXPECT_EQ(wal.last_seq(), n_threads * per);   // seq 总量对账
        EXPECT_EQ(wal.Close(), cabe::err::kSuccess);

        const auto got = RecoverCollect();
        ASSERT_EQ(got.size(), n_threads * per);
        VerifySeqDense(got);
        const auto m = KeySeqMap(got);
        VerifyPerThreadOrder(m, n_threads, per);
    }
}

// ---------------- 用例 C：自由跑（无节拍，覆盖屏障编排不到的随机交错） ----------------
TEST_F(WalConcurrencyTest, FreeRunDense) {
    ZeroWalRing();
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::WalSync);
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    const std::size_t n_threads = 8, per = 400;
    const auto keys = MakeKeys(n_threads, per);
    const auto rcs  = RunWriters(wal, keys, n_threads, per, /*barrier=*/false);
    for (std::size_t i = 0; i < rcs.size(); ++i) {
        ASSERT_EQ(rcs[i], cabe::err::kSuccess) << "第 " << i << " 个调用";
    }
    EXPECT_EQ(wal.last_seq(), n_threads * per);
    EXPECT_EQ(wal.Close(), cabe::err::kSuccess);

    const auto got = RecoverCollect();
    ASSERT_EQ(got.size(), n_threads * per);
    VerifySeqDense(got);
    const auto m = KeySeqMap(got);
    VerifyPerThreadOrder(m, n_threads, per);
}

// ---------------- 用例 D：单写者退化路径（批大小恒 1，与 P5 行为等价的功能面） ----------------
TEST_F(WalConcurrencyTest, SingleWriterDegenerate) {
    ZeroWalRing();
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::WalSync);
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    const std::size_t per = 300;
    const auto keys = MakeKeys(1, per);
    const auto rcs  = RunWriters(wal, keys, 1, per, /*barrier=*/false);
    for (std::size_t i = 0; i < per; ++i) {
        ASSERT_EQ(rcs[i], cabe::err::kSuccess);
    }
    EXPECT_EQ(wal.last_seq(), per);
    EXPECT_EQ(wal.Close(), cabe::err::kSuccess);

    const auto got = RecoverCollect();
    ASSERT_EQ(got.size(), per);
    VerifySeqDense(got);
    KeySeqMap(got);   // 恰好一次
}

// ---------------- 用例 E：撞墙并发——前缀持久兑现、拒绝帧零踪迹、seq 无洞 ----------------
TEST_F(WalConcurrencyTest, WallConcurrentPrefixSuffix) {
    ZeroWalRing();
    cabe::Options opts = MakeWalOpts(cabe::WalLevel::Async);   // 先攒批快速填环
    cabe::Wal wal;
    ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);

    // 填到只剩约 256 帧的空间：可用容量 = 环 − 4K（恒留一块）。
    const std::uint64_t ring = RingBytes();
    const std::size_t cap_frames =
        static_cast<std::size_t>((ring - cabe::kWalBlockSize) / cabe::kWalFrameSize);
    ASSERT_GT(cap_frames, std::size_t{1024});
    const std::size_t fill_frames = cap_frames - 256;
    std::vector<std::string> fill_keys(fill_frames);
    for (std::size_t i = 0; i < fill_frames; ++i) fill_keys[i] = "f" + std::to_string(i);
    for (std::size_t i = 0; i < fill_frames; ++i) {
        cabe::WalEntry e{};
        e.type = cabe::WalEntryType::Put; e.key = fill_keys[i];
        e.block = cabe::BlockId::Make(0, i & 0xFFFF); e.value_crc = 1; e.timestamp = 1;
        ASSERT_EQ(wal.WriteWal(e), cabe::err::kSuccess) << "填充第 " << i << " 帧";
    }
    ASSERT_EQ(wal.Flush(), cabe::err::kSuccess);   // 刷净攒批残留
    opts.wal_level = cabe::WalLevel::WalSync;      // 切档收紧（直驱版：先刷净再切）

    // 并发写到撞墙：每线程首个 kWalFull 即停（自由跑——屏障会被提前退场者卡死）。
    const std::size_t n_threads = 4, max_ops = 2000;
    const auto keys = MakeKeys(n_threads, max_ops);
    std::vector<int32_t> rcs(n_threads * max_ops, kNotSubmitted);
    std::vector<std::thread> ts;
    for (std::size_t tid = 0; tid < n_threads; ++tid) {
        ts.emplace_back([&, tid] {
            for (std::size_t i = 0; i < max_ops; ++i) {
                const std::size_t idx = tid * max_ops + i;
                cabe::WalEntry e{};
                e.type = cabe::WalEntryType::Put; e.key = keys[idx];
                e.block = cabe::BlockId::Make(0, idx & 0xFFFF); e.value_crc = 1; e.timestamp = 1;
                rcs[idx] = wal.WriteWal(e);
                if (rcs[idx] == cabe::err::kWalFull) break;   // 撞墙即停
            }
        });
    }
    for (auto& t : ts) t.join();

    // 每线程形态：若干成功 → 恰一个 kWalFull → 未提交；收集成功/拒绝 key 集。
    std::set<std::string> ok_keys, full_keys;
    std::size_t n_success = 0;
    for (std::size_t tid = 0; tid < n_threads; ++tid) {
        bool walled = false;
        for (std::size_t i = 0; i < max_ops; ++i) {
            const std::size_t idx = tid * max_ops + i;
            if (rcs[idx] == cabe::err::kSuccess) {
                ASSERT_FALSE(walled) << "撞墙之后同线程不应再有提交";
                ok_keys.insert(keys[idx]); ++n_success;
            } else if (rcs[idx] == cabe::err::kWalFull) {
                ASSERT_FALSE(walled);
                walled = true; full_keys.insert(keys[idx]);
            } else {
                ASSERT_EQ(rcs[idx], kNotSubmitted) << "意外结果码: " << rcs[idx];
                ASSERT_TRUE(walled) << "未提交只允许出现在撞墙之后";
            }
        }
        EXPECT_TRUE(walled) << "线程 " << tid << " 未撞到墙（环未满？）";
    }
    EXPECT_EQ(wal.Close(), cabe::err::kSuccess);

    // 盘面作证：重放数 = 填充 + 成功数；seq 稠密；成功 key 恰一次；kWalFull key 零踪迹。
    const auto got = RecoverCollect();
    ASSERT_EQ(got.size(), fill_frames + n_success);
    VerifySeqDense(got);
    const auto m = KeySeqMap(got);
    for (const auto& k : ok_keys)   EXPECT_EQ(m.count(k), 1u) << "成功帧未兑现: " << k;
    for (const auto& k : full_keys) EXPECT_EQ(m.count(k), 0u) << "被拒帧上了盘: " << k;
}

// ---------------- 用例 F：并发盘面 → 恢复续写 → 再恢复对账（与 M6 续写衔接兼容） ----------------
TEST_F(WalConcurrencyTest, RecoverContinueRecover) {
    ZeroWalRing();
    const std::size_t n_threads = 4, per = 100, extra = 50;

    {   // 阶段一：并发写
        cabe::Options opts = MakeWalOpts(cabe::WalLevel::WalSync);
        cabe::Wal wal;
        ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);
        const auto keys = MakeKeys(n_threads, per);
        const auto rcs  = RunWriters(wal, keys, n_threads, per, /*barrier=*/true);
        for (std::size_t i = 0; i < rcs.size(); ++i) ASSERT_EQ(rcs[i], cabe::err::kSuccess);
        EXPECT_EQ(wal.Close(), cabe::err::kSuccess);
    }

    std::vector<std::string> cont_keys(extra);
    for (std::size_t i = 0; i < extra; ++i) cont_keys[i] = "c-" + std::to_string(i);

    {   // 阶段二：恢复（锚帧续号衔接）→ 单线程续写
        cabe::Options opts = MakeWalRecoverOpts(cabe::WalLevel::WalSync);
        cabe::Wal wal;
        ASSERT_EQ(wal.Open(wal_, &opts), cabe::err::kSuccess);
        std::size_t replayed = 0;
        ASSERT_EQ(wal.Recover(0, [&](const cabe::WalEntry&, std::uint64_t) {
            ++replayed; return cabe::err::kSuccess;
        }), cabe::err::kSuccess);
        ASSERT_EQ(replayed, n_threads * per);
        for (std::size_t i = 0; i < extra; ++i) {
            cabe::WalEntry e{};
            e.type = cabe::WalEntryType::Put; e.key = cont_keys[i];
            e.block = cabe::BlockId::Make(0, i & 0xFFFF); e.value_crc = 1; e.timestamp = 1;
            ASSERT_EQ(wal.WriteWal(e), cabe::err::kSuccess);
        }
        EXPECT_EQ(wal.last_seq(), n_threads * per + extra);   // 稠密续号
        EXPECT_EQ(wal.Close(), cabe::err::kSuccess);
    }

    // 阶段三：全量再恢复对账——两段 key 各恰一次、seq 全程稠密、阶段一每线程序保持。
    const auto got = RecoverCollect();
    ASSERT_EQ(got.size(), n_threads * per + extra);
    VerifySeqDense(got);
    const auto m = KeySeqMap(got);
    VerifyPerThreadOrder(m, n_threads, per);
    for (std::size_t i = 0; i < extra; ++i) {
        EXPECT_EQ(m.count(cont_keys[i]), 1u) << "续写帧缺失: " << cont_keys[i];
    }
}
