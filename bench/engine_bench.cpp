/*
 * Project: Cabe
 * Engine end-to-end benchmark.
 *
 * Exercises the full Put/Get path including BufferPool, CRC32C,
 * O_DIRECT pwrite/pread, FreeList, and both index layers.
 *
 * Each benchmark uses its own backing file (sparse-allocated 8 GiB
 * on /var/tmp) to avoid cross-benchmark contamination and to have
 * headroom for long iteration runs.
 */
#include "engine/engine.h"
#include "common/structs.h"

#include <benchmark/benchmark.h>
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr off_t kBackingFileBytes = 8LL * 1024 * 1024 * 1024; // 8 GiB sparse

bool PrepareBackingFile(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    if (::ftruncate(fd, kBackingFileBytes) < 0) {
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }
    ::close(fd);
    // Sanity-check O_DIRECT support on the hosting filesystem.
    fd = ::open(path.c_str(), O_RDWR | O_DIRECT | O_SYNC);
    if (fd < 0) {
        ::unlink(path.c_str());
        return false;
    }
    ::close(fd);
    return true;
}

class EngineFixture : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        // 用规范化后的 benchmark 名作为文件唯一化后缀，
        // 崩溃后残留在 /var/tmp 的文件也能一眼对应回哪次跑的
        std::string name = state.name();
        std::replace(name.begin(), name.end(), '/', '_');
        devicePath_ = "/var/tmp/cabe_bench_" +
                      std::to_string(::getpid()) + "_" +
                      name + ".dat";

        if (!PrepareBackingFile(devicePath_)) {
            state.SkipWithError("backing file prep failed");
            return;
        }
        if (engine_.Open(devicePath_) != SUCCESS) {
            ::unlink(devicePath_.c_str());
            state.SkipWithError("Engine::Open failed");
            return;
        }
    }

    void TearDown(benchmark::State&) override {
        if (engine_.IsOpen()) engine_.Close();
        ::unlink(devicePath_.c_str());
    }

protected:
    Engine      engine_;
    std::string devicePath_;
};

// ----------------------------------------------------------------
// E1-E3: Put with varying value sizes.
// ----------------------------------------------------------------
BENCHMARK_DEFINE_F(EngineFixture, Put)(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>((i * 11 + 17) & 0xFF);
    }

    // 预生成 key 池：把 std::string 构造从计时循环里移走。
    // kKeyPoolSize 故意取小：1 KiB Put 在 Cabe 里仍占满 1 MiB chunk（定长块），
    // 8 GiB backing file 只能容纳 8192 个 chunk。如果 key 池太大永不循环，
    // 高 iter 会撑爆磁盘（500K iter × 1 MiB = 500 GiB）。
    // 用 4096 让 iter 数超过此值后立刻进入"覆盖写 + FreeList 回收"稳态，
    // 既测到了覆盖写路径，也避免磁盘耗尽。
    constexpr size_t kKeyPoolSize = 4096;
    std::vector<std::string> keys;
    keys.reserve(kKeyPoolSize);
    for (size_t k = 0; k < kKeyPoolSize; ++k) {
        keys.push_back("k_" + std::to_string(k));
    }

    size_t i = 0;
    for (auto _ : state) {
        const int32_t rc = engine_.Put(keys[i++ % kKeyPoolSize],
                                        {data.data(), data.size()});
        if (rc != SUCCESS) {
            state.SkipWithError("Put returned non-SUCCESS");
            break;
        }
    }
    state.SetBytesProcessed(state.iterations() * size);
}

BENCHMARK_REGISTER_F(EngineFixture, Put)
    ->Name("BM_Engine_Put")     // 对齐其他 bench 的 BM_* 命名前缀
    ->Arg(1024)                 // 1 KiB (pads to 1 MiB chunk)
    ->Arg(1024 * 1024)          // 1 MiB (exactly 1 chunk)
    ->Arg(16 * 1024 * 1024)     // 16 MiB (16 chunks)
    ->Unit(benchmark::kMicrosecond);

// ----------------------------------------------------------------
// E4: Get with varying value sizes.
// Pre-populates kPreload keys before the timed loop.
// ----------------------------------------------------------------
constexpr size_t kPreload = 64;

BENCHMARK_DEFINE_F(EngineFixture, Get)(benchmark::State& state) {
    const size_t size = static_cast<size_t>(state.range(0));
    std::vector<char> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>((i * 13 + 19) & 0xFF);
    }
    // 预生成 key 池，preload 与计时循环共用
    std::vector<std::string> keys;
    keys.reserve(kPreload);
    for (size_t k = 0; k < kPreload; ++k) {
        keys.push_back("k_" + std::to_string(k));
    }

    // 预写入（不计时）
    for (size_t k = 0; k < kPreload; ++k) {
        if (engine_.Put(keys[k], {data.data(), data.size()}) != SUCCESS) {
            state.SkipWithError("preload Put failed");
            return;
        }
    }

    std::vector<char> buf(size);
    uint64_t readSize = 0;
    size_t i = 0;
    for (auto _ : state) {
        const int32_t rc = engine_.Get(keys[i++ % kPreload],
                                        {buf.data(), buf.size()}, &readSize);
        benchmark::DoNotOptimize(buf);
        if (rc != SUCCESS) {
            state.SkipWithError("Get returned non-SUCCESS");
            break;
        }
    }
    state.SetBytesProcessed(state.iterations() * size);
}

BENCHMARK_REGISTER_F(EngineFixture, Get)
    ->Name("BM_Engine_Get")     // 对齐其他 bench 的 BM_* 命名前缀
    ->Arg(1024)
    ->Arg(1024 * 1024)
    ->Arg(16 * 1024 * 1024)
    ->Unit(benchmark::kMicrosecond);

} // namespace