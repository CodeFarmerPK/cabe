/*
* MetaIndex micro-benchmark.
 *
 * Measures Put and Get latency over unordered_map<string, KeyMeta>
 * at several population sizes (captures hash-table scaling behavior).
 */
#include "memory/meta_index.h"

#include <benchmark/benchmark.h>
#include <string>
#include <vector>

namespace {

    std::vector<std::string> MakeKeys(size_t count) {
        std::vector<std::string> keys;
        keys.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            keys.push_back("key_" + std::to_string(i));
        }
        return keys;
    }

    KeyMeta MakeMeta(uint64_t i) {
        return KeyMeta{
            .firstChunkId = i * 4,
            .chunkCount   = 4,
            .totalSize    = 4 * 1024 * 1024,
            .createdAt    = 0,
            .modifiedAt   = 0,
            .state        = DataState::Active,
        };
    }

    void BM_MetaIndex_Put(benchmark::State& state) {
        const size_t n = state.range(0);
        const auto keys = MakeKeys(n);
        MetaIndex idx;
        size_t i = 0;
        // 前 n 次迭代是真 insert，之后通过 i % n 复用 key
        // 变为覆盖写（overwrite）。google-benchmark 跑到时间目标
        // 通常会超过 n 次迭代，最终数字是 insert 与 overwrite 的
        // 混合，n 较大时以 overwrite 为主。
        for (auto _ : state) {
            idx.Put(keys[i % n], MakeMeta(i));
            ++i;
        }
        state.SetItemsProcessed(state.iterations());
    }

    void BM_MetaIndex_Get(benchmark::State& state) {
        const size_t n = state.range(0);
        const auto keys = MakeKeys(n);
        MetaIndex idx;
        for (size_t i = 0; i < n; ++i) {
            idx.Put(keys[i], MakeMeta(i));
        }

        size_t i = 0;
        KeyMeta out{};
        for (auto _ : state) {
            idx.Get(keys[i % n], &out);
            benchmark::DoNotOptimize(out);
            ++i;
        }
        state.SetItemsProcessed(state.iterations());
    }

} // namespace

BENCHMARK(BM_MetaIndex_Put)->RangeMultiplier(100)->Range(1'000, 1'000'000);
BENCHMARK(BM_MetaIndex_Get)->RangeMultiplier(100)->Range(1'000, 1'000'000);