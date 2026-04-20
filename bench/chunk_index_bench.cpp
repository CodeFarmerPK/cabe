/*
* ChunkIndex micro-benchmark.
 *
 * Measures GetRange over std::map<ChunkId, ChunkMeta>. This is the
 * read path for multi-chunk Get, whose cost scales with the number
 * of chunks spanned.
 */
#include "memory/chunk_index.h"

#include <benchmark/benchmark.h>
#include <vector>

namespace {

    constexpr uint32_t kMapSize = 100'000;

    void BM_ChunkIndex_GetRange(benchmark::State& state) {
        const auto count = static_cast<uint32_t>(state.range(0));

        ChunkIndex idx;
        for (uint32_t i = 0; i < kMapSize; ++i) {
            idx.Put(i, ChunkMeta{
                .blockId   = i,
                .crc       = 0,
                .timestamp = 0,
                .state     = DataState::Active,
            });
        }

        std::vector<ChunkMeta> out;
        out.reserve(count);

        uint32_t start = 0;
        // count 来自 state.range(0)，恒 >= 1
        const uint32_t step = count;
        for (auto _ : state) {
            idx.GetRange(start, count, &out);
            benchmark::DoNotOptimize(out);
            start += step;
            if (start + count > kMapSize) start = 0;
        }
        state.SetItemsProcessed(state.iterations() * count);
    }

} // namespace

BENCHMARK(BM_ChunkIndex_GetRange)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1024)
    ->Unit(benchmark::kNanosecond);