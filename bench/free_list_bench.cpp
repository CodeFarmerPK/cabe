/*
* FreeList micro-benchmark.
 *
 * Two scenarios:
 *   1. Allocate-only (simulates Put-heavy workload, never releases)
 *   2. Allocate + Release (simulates steady-state after GC)
 *
 * Pre-P1.2 baseline for the mutex we'll add.
 */
#include "storage/free_list.h"

#include <benchmark/benchmark.h>

namespace {

    void BM_FreeList_Allocate(benchmark::State& state) {
        FreeList fl;
        BlockId id = 0;
        for (auto _ : state) {
            fl.Allocate(&id);
            benchmark::DoNotOptimize(id);
        }
        state.SetItemsProcessed(state.iterations());
    }

    void BM_FreeList_AllocateRelease(benchmark::State& state) {
        FreeList fl;
        BlockId id = 0;
        for (auto _ : state) {
            fl.Allocate(&id);
            fl.Release(id);
        }
        state.SetItemsProcessed(state.iterations());
    }

} // namespace

BENCHMARK(BM_FreeList_Allocate)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FreeList_AllocateRelease)->Unit(benchmark::kNanosecond);