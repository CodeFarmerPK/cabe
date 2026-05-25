#include "engine/engine.h"

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string GetTestDevice() {
    const char* dev = std::getenv("CABE_TEST_DEVICE");
    return dev ? std::string(dev) : "";
}

std::vector<std::byte> MakeValue(std::byte fill) {
    std::vector<std::byte> v(cabe::kValueSize);
    std::memset(v.data(), static_cast<int>(fill), cabe::kValueSize);
    return v;
}

class EngineBench : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        device_ = GetTestDevice();
        if (device_.empty()) {
            state.SkipWithMessage("CABE_TEST_DEVICE 未设置");
            return;
        }
        auto s = engine_.Open(cabe::Options{{cabe::DeviceConfig{device_}}});
        if (!s.ok()) {
            state.SkipWithMessage("Engine::Open 失败");
            return;
        }
    }

    void TearDown(benchmark::State&) override {
        engine_.Close();
    }

protected:
    cabe::Engine engine_;
    std::string device_;
};

} // namespace

BENCHMARK_DEFINE_F(EngineBench, BM_Put)(benchmark::State& state) {
    auto value = MakeValue(std::byte{0xAB});
    int64_t seq = 0;
    for (auto _ : state) {
        std::string key = "put-" + std::to_string(seq++);
        auto s = engine_.Put(key, cabe::DataView{value});
        if (!s.ok()) {
            // 写满 → 关闭重开重置 FreeList
            engine_.Close();
            engine_.Open(cabe::Options{{cabe::DeviceConfig{device_}}});
            seq = 0;
        }
        benchmark::ClobberMemory();
    }
    state.SetBytesProcessed(state.iterations() * cabe::kValueSize);
}
BENCHMARK_REGISTER_F(EngineBench, BM_Put)->Unit(benchmark::kMillisecond);

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
BENCHMARK_REGISTER_F(EngineBench, BM_Get)->Unit(benchmark::kMillisecond);

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
            // 删完了 → 重新写一批
            ++round;
            for (int i = 0; i < kBatch; ++i) {
                engine_.Put("del-" + std::to_string(i), cabe::DataView{value});
            }
            idx = 0;
            continue;
        }
        ++idx;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(EngineBench, BM_Delete)->Unit(benchmark::kMillisecond);
