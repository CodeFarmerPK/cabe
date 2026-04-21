/*
 * CRC32C micro-benchmark.
 *
 * On Fedora 43 x86_64 (SSE4.2 guaranteed) this exercises the
 * hardware path via _mm_crc32_u64. Expected throughput ~30 GB/s.
 *
 * Sizes cover the regimes the Engine actually hits after the
 * "CRC only covers chunkSize" change (see engine.cpp):
 *   - 1 KiB  近似末 chunk 半满场景下的 CRC 覆盖范围下界
 *   - 1 MiB  单个满 chunk
 *   - 16 MiB 多 chunk 聚合（16 chunks）
 */
#include "util/crc32.h"

#include <benchmark/benchmark.h>
#include <cstdint>
#include <vector>

namespace {

    void BM_CRC32(benchmark::State& state) {
        const size_t size = static_cast<size_t>(state.range(0));
        std::vector<char> data(size);
        // Deterministic non-trivial pattern (avoids zero-run shortcuts).
        for (size_t i = 0; i < size; ++i) {
            data[i] = static_cast<char>((i * 7 + 13) & 0xFF);
        }

        for (auto _ : state) {
            uint32_t crc = cabe::util::CRC32({data.data(), data.size()});
            benchmark::DoNotOptimize(crc);
        }
        state.SetBytesProcessed(state.iterations() * size);
    }

} // namespace

BENCHMARK(BM_CRC32)
    ->Arg(1024)               // 1 KiB（自动单位：ns）
    ->Arg(1024 * 1024)        // 1 MiB（自动单位：μs）
    ->Arg(16 * 1024 * 1024);  // 16 MiB（自动单位：μs）