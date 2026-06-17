#include "engine/engine.h"

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string GetEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : "";
}

std::vector<std::byte> MakeValue(std::byte fill) {
    std::vector<std::byte> v(cabe::kValueSize);
    std::memset(v.data(), static_cast<int>(fill), cabe::kValueSize);
    return v;
}

class EngineBench : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_  = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            state.SkipWithMessage("需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE");
            return;
        }
        // P6M3：WAL 级别由 state.range(0) 参数化（1/2/3/4），输出名带级别（如 BM_Put/3）。
        const int64_t lvl = state.range(0);
        if (!cabe::IsValidWalLevel(static_cast<cabe::WalLevel>(lvl))) {
            state.SkipWithMessage("WAL 级别非法（须 1/2/3/4）");
            return;
        }
        // 首次 create 格式化设备组；BM_Put 写满后重开同样用 create 重新格式化——
        // P5M6 起 recover 是真恢复（索引/分配器从盘上重建），写满后 recover 重开会回到
        // "索引满 + 空闲零"的状态、Put 永远 kEngineNoSpace；重置语义只有 create 提供。
        // P6M3：bench 用大设备（mkloop create-bench：value 32G），写 ≤20G 永不写满 →
        //   重开实际不触发，仅作安全网（见 P6M3 稿 §5）。
        auto s = engine_.Open(MakeOpts(/*create=*/true, static_cast<int>(lvl)));
        if (!s.ok()) {
            state.SkipWithMessage("Engine::Open 失败");
            return;
        }
    }

    void TearDown(benchmark::State&) override {
        engine_.Close();
    }

protected:
    cabe::Options MakeOpts(bool create, int level) {
        cabe::Options opts;
        opts.devices.push_back({data_, wal_, snap_});
        opts.create    = create;
        opts.wal_level = static_cast<cabe::WalLevel>(level);   // P6M3：按参数化级别（1/2/3/4）
        // P6M3：bench 快照阈值 1M——20G 写入 ≈ 2.6M WAL 帧，阈值 1M 让快照在测试内真实触发
        //   几次（每 ~8192 Put 一次），把周期快照开销计入吞吐（快照是真实负载）。bench WAL
        //   设备 1G，容量校验 环 ≥ 阈值×2=2M 轻松过。（原 4M 阈值在 ≤20G 写入内不触发快照。）
        opts.snapshot_threshold_bytes = 1ull * 1024 * 1024;
        return opts;
    }

    cabe::Engine engine_;
    std::string data_, wal_, snap_;
};

} // namespace

BENCHMARK_DEFINE_F(EngineBench, BM_Put)(benchmark::State& state) {
    auto value = MakeValue(std::byte{0xAB});
    int64_t seq = 0;
    for (auto _ : state) {
        std::string key = "put-" + std::to_string(seq++);
        auto s = engine_.Put(key, cabe::DataView{value});
        if (!s.ok()) {
            // 写满 → 关闭重开（安全网；P6M3 大设备下不触发，见 §5）。重开用同一 WAL 级别。
            engine_.Close();
            engine_.Open(MakeOpts(/*create=*/true, static_cast<int>(state.range(0))));
            seq = 0;
        }
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * cabe::kValueSize);
}
// P6M3：按 WAL 级别参数化（1/2/3/4），输出名 BM_Put/1..BM_Put/4。
// UseRealTime（三个 bench 同此）：Put/Get/Delete 都是 I/O 密集——每次操作 = 1MiB 写 + fdatasync，
//   时间几乎全花在 D 态等盘、CPU 时间近零。若按 google-benchmark 默认的 CPU 时间满足 min_time，
//   框架会以为"操作超快"而疯狂加大迭代数（永远凑不够 CPU 时间）→ 单配置写满设备触发重开、整轮
//   跑不完；且 bytes/items_per_second 按 CPU 时间算会严重虚高。改按墙钟：迭代数有界、吞吐 = 真实
//   bytes/墙钟（与 wal_concurrency 的 UseRealTime 同理）。
BENCHMARK_REGISTER_F(EngineBench, BM_Put)->Unit(benchmark::kMillisecond)->Arg(1)->Arg(2)->Arg(3)->Arg(4)->UseRealTime();

BENCHMARK_DEFINE_F(EngineBench, BM_Get)(benchmark::State& state) {
    // 预写 8 个 key
    auto value = MakeValue(std::byte{0xCD});
    constexpr int kPrewrite = 8;
    for (int i = 0; i < kPrewrite; ++i) {
        engine_.Put("get-" + std::to_string(i), cabe::DataView{value});
    }

    std::vector<std::byte> out(cabe::kValueSize);
    int64_t seq = 0;
    for (auto _ : state) {
        std::string key = "get-" + std::to_string(seq % kPrewrite);
        benchmark::DoNotOptimize(
            engine_.Get(key, cabe::DataBuffer{out}));
        benchmark::ClobberMemory();
        ++seq;
    }
    state.SetBytesProcessed(state.iterations() * cabe::kValueSize);
}
// P6M3：按 WAL 级别参数化。Get 读路径不碰 WAL、级别无关——4 档应趋同（交叉验证证据）。
// UseRealTime 理由见 BM_Put 注释（I/O 密集，按墙钟计时）。
BENCHMARK_REGISTER_F(EngineBench, BM_Get)->Unit(benchmark::kMillisecond)->Arg(1)->Arg(2)->Arg(3)->Arg(4)->UseRealTime();

BENCHMARK_DEFINE_F(EngineBench, BM_Delete)(benchmark::State& state) {
    auto value = MakeValue(std::byte{0xEF});
    int64_t round = 0;
    constexpr int kBatch = 8;

    // 预写一批
    for (int i = 0; i < kBatch; ++i) {
        engine_.Put("del-" + std::to_string(i), cabe::DataView{value});
    }

    int idx = 0;
    for (auto _ : state) {
        std::string key = "del-" + std::to_string(idx);
        auto s = engine_.Delete(key);
        if (!s.ok()) {
            // 删空 → 重填一批（测试补数据、非被测操作）。P6M3：用 PauseTiming 排除出计时——
            // 否则重填的 Put（写 value、级别相关，级别 1 FUA 慢）会污染 Delete 测量，破坏
            // "Delete 1≡3/2≡4"交叉验证。（与 BM_Put 重开不同：重开靠大设备规避；重填是
            // Delete 必需补数据、无法靠设备规避，正该 PauseTiming。）
            // 实测注（M3 io_uring 冒烟）：级别 4 异步重填的 value 写不立即落盘，PauseTiming 能排
            // 除其 inline 耗时、却排不掉它在后台 writeback 集中刷出时对后续被测 Delete 的停顿尖峰
            // ——故 loop 设备上 Delete 2≢4。属宿主机伪影、非缺陷；M4 量测须规避，见 P6M3 §3.3。
            state.PauseTiming();
            ++round;
            for (int i = 0; i < kBatch; ++i) {
                engine_.Put("del-" + std::to_string(i), cabe::DataView{value});
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
// P6M3：按 WAL 级别参数化。Delete 无 value——级别 1≡3（同步）、2≡4（攒批），4 档应呈两对相等（交叉验证）。
// UseRealTime 理由见 BM_Put 注释（I/O 密集，按墙钟计时）。
BENCHMARK_REGISTER_F(EngineBench, BM_Delete)->Unit(benchmark::kMillisecond)->Arg(1)->Arg(2)->Arg(3)->Arg(4)->UseRealTime();
