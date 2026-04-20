/*
* BufferPool micro-benchmark.
 *
 * Measures: one Acquire + one Release round-trip.
 *
 * Acquire() does Init() + memset the buffer to zero, so this
 * benchmark effectively measures:
 *   1) freeStack pop/push cost (~ns)
 *   2) 1 MiB memset cost (dominant, ~50 μs on modern CPU)
 */
#include "buffer/buffer_pool.h"
#include "common/structs.h"

#include <benchmark/benchmark.h>

namespace {

    constexpr size_t   kBufSize   = CABE_VALUE_DATA_SIZE; // 1 MiB
    constexpr uint32_t kBufCount  = 64;

    void BM_BufferPool_AcquireRelease(benchmark::State& state) {
        BufferPool pool;
        if (pool.Init(kBufSize, kBufCount) != SUCCESS) {
            state.SkipWithError("BufferPool::Init failed");
            return;
        }
        for (auto _ : state) {
            char* p = pool.Acquire();
            benchmark::DoNotOptimize(p);
            pool.Release(p);
        }
        state.SetBytesProcessed(state.iterations() * kBufSize);
    }

} // namespace

BENCHMARK(BM_BufferPool_AcquireRelease)->Unit(benchmark::kMicrosecond);