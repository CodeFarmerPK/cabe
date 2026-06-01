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
        // 首次 create 格式化设备组；BM_Put 写满后重开用 recover 仅重置内存 FreeList
        auto s = engine_.Open(MakeOpts(/*create=*/true));
        if (!s.ok()) {
            state.SkipWithMessage("Engine::Open 失败");
            return;
        }
    }

    void TearDown(benchmark::State&) override {
        engine_.Close();
    }

protected:
    cabe::Options MakeOpts(bool create) {
        cabe::Options opts;
        opts.devices.push_back({data_, wal_, snap_});
        opts.create = create;
        // 不设 wal_level → 默认级别 3（WalSync：WAL 同步、value 异步，不 FUA）。
        // 注意：M2 时引擎强制级别 1（value 也 FUA），故 M3 后 BM_Put 少一次 value fdatasync，
        //   数字会比 M2 基线好——对比历史基准时须知此为持久级别变化，非纯代码优化。
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
            // 写满 → 关闭重开（recover，仅重置内存 FreeList，不重新格式化）
            engine_.Close();
            engine_.Open(MakeOpts(/*create=*/false));
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
