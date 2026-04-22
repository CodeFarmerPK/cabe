/*
* MetaIndex micro-benchmark.
 *
 * Measures Put and Get latency over unordered_map<string, KeyMeta>
 * at several population sizes (captures hash-table scaling behavior).
 *
 * key 分布说明：
 *   旧版本用 "key_0", "key_1", ... 这种 monotonic 数字串，对
 *   std::hash<std::string> 来说会形成 hot bucket，bench 偏乐观。
 *   现在改用固定种子 PRNG 生成 16 字节 hex 串（近似 UUID 短哈希），
 *   分布接近真实业务 key（如 photo_id_hash、object_id），
 *   bench 数字更接近生产环境表现。
 *   固定种子保证可复现 / 可对比。
 */
#include "memory/meta_index.h"

#include <benchmark/benchmark.h>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace {

    std::vector<std::string> MakeKeys(size_t count) {
        std::vector<std::string> keys;
        keys.reserve(count);
        // 固定种子 mt19937_64，bench 间可复现
        std::mt19937_64                              rng(0xCABE'C0DE'1234'5678ULL);
        std::uniform_int_distribution<uint64_t>      dist;
        constexpr char                               hex[] = "0123456789abcdef";
        for (size_t i = 0; i < count; ++i) {
            const uint64_t r = dist(rng);
            std::string    key(16, '0');
            for (int b = 0; b < 16; ++b) {
                key[b] = hex[(r >> (b * 4)) & 0xF];
            }
            keys.push_back(std::move(key));
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