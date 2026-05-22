#include "util/hash.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <span>
#include <vector>

static void BM_Hash(benchmark::State &state) {
    std::vector<std::byte> buf(static_cast<std::size_t>(state.range(0)), std::byte{0xCD});
    const cabe::DataView dv{buf};
    for (auto _ : state) {
        benchmark::DoNotOptimize(cabe::util::Hash(dv));
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Hash)->Arg(16)->Arg(256)->Arg(1 << 20);
