/*
 * Project: Cabe
 * Engine end-to-end benchmark(裸设备语义)。
 *
 * Exercises the full Put/Get path including BufferPool, CRC32C,
 * O_DIRECT pwrite/pread, FreeList, and both index layers.
 *
 * 裸设备依赖:Cabe 直接操作裸块设备,bench 通过 CABE_BENCH_DEVICE 环境变量
 * 指定 backing。建议用足够大的设备(>= 16 GiB)以便 BM_Engine_Put 16 MiB
 * 长迭代不耗尽空间——FreeList 复用路径在覆盖写到达 keyPool 上限后才会激活。
 *
 *   sudo CABE_BENCH_DEVICE=/dev/loop0 ./scripts/run-bench.sh
 *
 * 未设置时所有 bench 自动 SkipWithError。
 *
 * 注意:所有 bench 共享同一设备(只有一个真实 backing)。每个 bench 的
 * SetUp 都新建 Engine 并 Open,内存索引从空开始,nextBlockId_ 从 0 重新
 * 覆盖写设备,bench 之间不残留逻辑状态。
 */
#include "engine/engine.h"
#include "common/structs.h"

#include <benchmark/benchmark.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

    std::string GetBenchDevice() {
        const char* env = std::getenv("CABE_BENCH_DEVICE");
        if (env == nullptr || *env == '\0') return {};
        return env;
}

class EngineFixture : public benchmark::Fixture {
public:
    // 为什么用 unique_ptr<Engine> 而不是值类型:
    //   ::Engine 的生命周期契约是 Open/Close **一次性**(Close 后 usedOnce_
    //   置 true,后续同实例 Open 会返回 ENGINE_INSTANCE_USED)。
    //   Google Benchmark 在单个 Arg 的一次测量里,会为了校准迭代次数多次
    //   调用 Fixture::SetUp / TearDown 到同一个 fixture 实例上。若 engine_
    //   是值成员,第一次 TearDown 的 Close 就让后续所有 SetUp 的 Open 挂掉。
    //   改用 unique_ptr 后,每次 SetUp 都 make_unique 出一个**全新的** Engine
    //   实例,usedOnce_ 重新从 false 开始;TearDown 显式 reset 销毁。这和
    //   cabe_engine_bench.cpp 的 CabeEngineFixture 模式一致。
    void SetUp(benchmark::State& state) override {
        devicePath_ = GetBenchDevice();
        if (devicePath_.empty()) {
            state.SkipWithError("CABE_BENCH_DEVICE not set; "
                                "use scripts/mkloop.sh and `export CABE_BENCH_DEVICE=/dev/loopX`");
            return;
        }
        engine_ = std::make_unique<Engine>();
        if (const int32_t rc = engine_->Open(devicePath_); rc != SUCCESS) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "Engine::Open failed, code=%d path=%s",
                          rc, devicePath_.c_str());
            state.SkipWithError(msg);
        }
    }

    void TearDown(benchmark::State&) override {
        if (engine_ && engine_->IsOpen()) engine_->Close();
        engine_.reset();   // 销毁当前 Engine 实例,下次 SetUp 拿到全新的
        // 不 unlink:裸设备由调用方管理(/dev/loop0 的生命周期由 mkloop.sh 负责)
    }

protected:
    std::unique_ptr<Engine> engine_;
    std::string             devicePath_;
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

    // SetUp 失败时 engine_ 可能是 nullptr(CABE_BENCH_DEVICE 未设)或
    // 非空但未 Open(Open 返错)。Google Benchmark 在 SkipWithError 后仍会
    // 进到 body,所以 state 循环前先判断,避免解引用空指针。
    if (!engine_ || !engine_->IsOpen()) {
        return;
    }
    size_t i = 0;
    for (auto _ : state) {
        const int32_t rc = engine_->Put(keys[i++ % kKeyPoolSize],
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
    // SetUp 失败时 engine_ 为 nullptr 或未 Open。preload 在 state 循环之前,
    // SkipWithError 挡不住 —— 显式判空,直接 return。
    if (!engine_ || !engine_->IsOpen()) {
        return;
    }
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
        if (engine_->Put(keys[k], {data.data(), data.size()}) != SUCCESS) {
            state.SkipWithError("preload Put failed");
            return;
        }
    }

    std::vector<char> buf(size);
    uint64_t readSize = 0;
    size_t i = 0;
    for (auto _ : state) {
        const int32_t rc = engine_->Get(keys[i++ % kPreload],
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