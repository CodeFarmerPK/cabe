#include "util/crc32.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <span>
#include <vector>

static void BM_CRC32(benchmark::State &state) {
    std::vector<std::byte> buf(static_cast<std::size_t>(state.range(0)), std::byte{0xAB});
    const cabe::DataView dv{buf};
    for (auto _ : state) {
        benchmark::DoNotOptimize(buf.data());                  // 让编译器视 buf 运行时可变，禁 hoist
        benchmark::DoNotOptimize(cabe::util::CRC32(dv));
        benchmark::ClobberMemory();                            // memory barrier，禁跨迭代缓存结果
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_CRC32)->Arg(64)->Arg(4096)->Arg(1 << 20); // 1<<20 = kValueSize(1 MiB)
