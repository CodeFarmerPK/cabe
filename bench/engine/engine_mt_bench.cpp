// P7：多设备 + 并发 engine 吞吐微基准。
// 测 P7 的核心贡献——per-device reactor 无锁并发 + 多设备 hash 路由的扩展性
// （现成 bench_engine 是单设备单线程，测不出这块）。
//
// 与 P6 对比（P6 基线是单设备单线程 io_uring）：每个 op 用 Threads(1/2/4/8) 扫并发——
//   Threads(1) ≈ 单设备单线程（1 线程串行、同一时刻仅 1 op 在飞、只喂饱 1 个 reactor）
//     → 对 P6 单设备数、覆盖 P7-D2 的「单线程 p50 ≤ P6+10%」红线；
//   Threads(2+) 出双设备聚合 QPS（≥2 线程喂饱 2 个 reactor 并行）
//     → 聚合 QPS ÷ (2 × Threads(1) QPS) = 扩展效率，对 P7-D2 的「多线程 QPS ≥ 70%×N」红线（N=2）。
//   （P7 每设备一个 reactor 串行执行，扩展来自设备数 N、不是单设备堆线程；故 Threads > N 会在 N 个
//    reactor 上封顶——曲线在 Threads=2 抬升、之后趋平即预期。）
//
// 设备：2 组，读 CABE_TEST_DEVICE[/2]/WAL[/2]/SNAPSHOT[/2]
//   （用 ./scripts/mkloop.sh create-bench-multi 的两组大稀疏设备；run-bench.sh 用 --device2… 导出）。
// WAL 级别：CABE_BENCH_WAL_LEVEL（默认 3=WalSync，同步档 I/O 密集、最能显设备并行；
//   各级别的单线程 p50 由 bench_engine 覆盖，本基准固定一档专看并发扩展）。
// 聚合：SetItemsProcessed/SetBytesProcessed + UseRealTime（框架按墙钟跨线程求和 → 聚合 QPS/带宽）。
//
// loop 盘 caveat：两组共享同一背存 → 设备并行是假的、绝对与扩展数值都不可信（真盘扩展留 P11）；
//   本基准用于观察「并发 + 多设备路径正确工作 + 相对趋势」，不作形式化 p7 基线。
// 设计依据：P7-D2 红线；bench 范式沿用 bench/wal/wal_concurrency_bench.cpp（thread 0 开共享资源 +
//   框架计时屏障）与 bench/engine/engine_bench.cpp（op 体、UseRealTime、快照阈值）。

#include "engine/engine.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string GetEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : "";
}

// WAL 级别可经 CABE_BENCH_WAL_LEVEL 覆盖（默认 3=WalSync）；越界回落 3。
int BenchWalLevel() {
    const std::string s = GetEnv("CABE_BENCH_WAL_LEVEL");
    if (s.empty()) return 3;
    const int lvl = std::atoi(s.c_str());
    return (lvl >= 1 && lvl <= 4) ? lvl : 3;
}

std::vector<std::byte> MakeValue(std::byte fill) {
    std::vector<std::byte> v(cabe::kValueSize);
    std::memset(v.data(), static_cast<int>(fill), cabe::kValueSize);
    return v;
}

constexpr int kMaxThreads = 8;     // Threads() 最大档；Delete 预写按此覆盖每个潜在线程
constexpr int kGetKeys    = 64;    // Get 共享 key 集大小（hash%2 自然散到两盘）
constexpr int kDelBatch   = 64;    // Delete 每线程一批的大小

// 基类：仅 0 号线程开「2 组设备」的引擎（fresh create）。框架计时屏障保证其余线程过屏障前引擎
//   已开且可见（同 wal_concurrency_bench）。引擎跨线程共享——P7 引擎本就线程安全，并发正是被测对象。
class EngineMtBase : public benchmark::Fixture {
public:
    void TearDown(benchmark::State& state) override {
        if (state.thread_index() == 0 && open_ok_) engine_.Close();
    }

protected:
    bool OpenTwoDevices(benchmark::State& state) {
        const std::string d1 = GetEnv("CABE_TEST_DEVICE"),  w1 = GetEnv("CABE_TEST_WAL_DEVICE"),
                          s1 = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        const std::string d2 = GetEnv("CABE_TEST_DEVICE2"), w2 = GetEnv("CABE_TEST_WAL_DEVICE2"),
                          s2 = GetEnv("CABE_TEST_SNAPSHOT_DEVICE2");
        if (d1.empty() || w1.empty() || s1.empty() || d2.empty() || w2.empty() || s2.empty()) {
            state.SkipWithMessage("需两组设备 CABE_TEST_*[ 与 *2]（用 mkloop create-bench-multi）");
            return false;
        }
        cabe::Options opts;
        opts.devices.push_back({d1, w1, s1});
        opts.devices.push_back({d2, w2, s2});
        opts.create    = true;                                 // 每配置 fresh 格式化（重置语义，同 bench_engine）
        opts.wal_level = static_cast<cabe::WalLevel>(BenchWalLevel());
        opts.snapshot_threshold_bytes = 1ull * 1024 * 1024;    // 让快照在测内真实触发（同 bench_engine）
        const auto s = engine_.Open(opts);
        if (!s.ok()) {
            state.SkipWithMessage("Engine::Open 2-device 失败（检查两组大 bench 设备容量）");
            return false;
        }
        open_ok_ = true;
        return true;
    }

    cabe::Engine engine_;      // 跨线程共享
    bool open_ok_ = false;
};

class EngineMtPut : public EngineMtBase {
public:
    void SetUp(benchmark::State& state) override {
        if (state.thread_index() == 0) OpenTwoDevices(state);
    }
};

class EngineMtGet : public EngineMtBase {
public:
    void SetUp(benchmark::State& state) override {
        if (state.thread_index() != 0) return;
        if (!OpenTwoDevices(state)) return;
        const auto val = MakeValue(std::byte{0xCD});           // 预写共享 key 集（读不改状态、并发安全）
        for (int i = 0; i < kGetKeys; ++i) {
            engine_.Put("mtget-" + std::to_string(i), cabe::DataView{val});
        }
    }
};

class EngineMtDelete : public EngineMtBase {
public:
    void SetUp(benchmark::State& state) override {
        if (state.thread_index() != 0) return;
        if (!OpenTwoDevices(state)) return;
        const auto val = MakeValue(std::byte{0xEF});           // 每（潜在）线程预写一批，各删各的、互不干扰
        for (int t = 0; t < kMaxThreads; ++t) {
            for (int i = 0; i < kDelBatch; ++i) {
                engine_.Put("mtdel-" + std::to_string(t) + "-" + std::to_string(i), cabe::DataView{val});
            }
        }
    }
};

}  // namespace

// Put：每线程写自己命名空间的 key → 经 hash%2 自然散到两盘。框架跨线程求和 → 聚合 QPS/带宽。
BENCHMARK_DEFINE_F(EngineMtPut, BM_MtPut)(benchmark::State& state) {
    const auto value = MakeValue(std::byte{0xAB});
    const int tid = state.thread_index();
    std::int64_t seq = 0;
    for (auto _ : state) {
        const std::string key = "mtput-" + std::to_string(tid) + "-" + std::to_string(seq++);
        const auto s = engine_.Put(key, cabe::DataView{value});
        if (!s.ok()) {
            state.SkipWithMessage("Put 失败（设备写满？用大 bench 设备 / 缩 --benchmark_min_time）");
            break;
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * cabe::kValueSize);
}
BENCHMARK_REGISTER_F(EngineMtPut, BM_MtPut)
    ->Unit(benchmark::kMillisecond)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)
    ->UseRealTime();

// Get：所有线程从共享 key 集轮读（并发读安全）。读路径不碰 WAL、级别无关。
BENCHMARK_DEFINE_F(EngineMtGet, BM_MtGet)(benchmark::State& state) {
    std::vector<std::byte> out(cabe::kValueSize);
    std::int64_t seq = 0;
    for (auto _ : state) {
        const std::string key = "mtget-" + std::to_string(seq % kGetKeys);
        benchmark::DoNotOptimize(engine_.Get(key, cabe::DataBuffer{out}));
        benchmark::ClobberMemory();
        ++seq;
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * cabe::kValueSize);
}
BENCHMARK_REGISTER_F(EngineMtGet, BM_MtGet)
    ->Unit(benchmark::kMillisecond)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)
    ->UseRealTime();

// Delete：每线程删自己那批；删空 → PauseTiming 重填自己的批（补数据非被测，同 bench_engine）。
//   注：MT 下 PauseTiming 为每线程、UseRealTime 按整体墙钟——loop 上 Delete 数值最粗（重填 writeback
//   尖峰排不净，见 P6M3 §3.3），仅作并发路径 / 相对趋势观察。
BENCHMARK_DEFINE_F(EngineMtDelete, BM_MtDelete)(benchmark::State& state) {
    const auto value = MakeValue(std::byte{0xEF});
    const std::string prefix = "mtdel-" + std::to_string(state.thread_index()) + "-";
    int idx = 0;
    for (auto _ : state) {
        const auto s = engine_.Delete(prefix + std::to_string(idx));
        if (!s.ok()) {                                   // 本批删空（kIndexKeyNotFound）→ 重填自己的批
            state.PauseTiming();
            for (int i = 0; i < kDelBatch; ++i) {
                engine_.Put(prefix + std::to_string(i), cabe::DataView{value});
            }
            idx = 0;
            state.ResumeTiming();
            continue;
        }
        ++idx;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(EngineMtDelete, BM_MtDelete)
    ->Unit(benchmark::kMillisecond)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)
    ->UseRealTime();
