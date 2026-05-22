#include "util/crc32.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <span>
#include <vector>

static void BM_CRC32(benchmark::State &state) {
    std::vector<std::byte> buf(static_cast<std::size_t>(state.range(0)), std::byte{0xAB});
    const cabe::DataView dv{buf};
    for (auto _ : state) {
        benchmark::DoNotOptimize(cabe::util::CRC32(dv));
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CRC32)->Arg(64)->Arg(4096)->Arg(1 << 20); // 1<<20 = kValueSize(1 MiB)
