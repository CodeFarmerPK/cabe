/*
 * Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 *
 * P2 公开 API（cabe::Engine）端到端 + 并发 benchmark。
 *
 * 与 engine_bench.cpp 的区别：
 *   - 此文件测 `cabe::Engine`（公开 API），覆盖 Pimpl / TranslateStatus /
 *     std::string_view → std::string 拷贝 / std::span<std::byte> → DataView 转换 等开销
 *   - engine_bench.cpp 测的是内部 `::Engine`，绕过上述 API 层
 *
 * 用途：
 *   1. 量化 P2 API 层引入的额外开销（与 internal bench 对比即可看到）
 *   2. 为 P3+ API 演进（如改 Get 输出类型、加 zero-copy 接口）提供 baseline
 */
#include "cabe/cabe.h"
#include "common/structs.h"

#include <benchmark/benchmark.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// 裸设备 bench:同 engine_bench,需要 CABE_BENCH_DEVICE 环境变量
constexpr size_t kKeyPoolSize = 4096;
constexpr size_t kPreloadKeys = 256;

// 并发 bench 的 BufferPool 容量 = 2 × ThreadRange 上限。
// 当前 BM_CabeEngine_ConcurrentGet 用 ThreadRange(1, 16),上限 16 线程,
// pool 取 32 给足 headroom —— 避免 16 个线程同时 Acquire 时后半部分撞上
// POOL_EXHAUSTED 把 bench 打死(BufferPool::Acquire 目前是非阻塞语义)。
// 如果未来 ThreadRange 扩到 32 线程,记得同步更新此常量。
constexpr uint32_t kConcurrentPoolSize = 32;


std::string GetBenchDevice() {
    const char* env = std::getenv("CABE_BENCH_DEVICE");
    if (env == nullptr || *env == '\0') return {};
    return env;
}

    cabe::Options DefaultOptions(const std::string& devicePath) {
    cabe::Options opts;
    opts.device_path       = devicePath;
    opts.buffer_pool_count = 8;
    return opts;
}

std::vector<std::byte> MakeBytes(size_t n, uint8_t seed = 0x41) {
    std::vector<std::byte> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::byte>((i * 11 + seed) & 0xFF);
    }
    return v;
}

// ----------------------------------------------------------------
// Fixture:所有 bench 共享 CABE_BENCH_DEVICE 指定的裸设备
// ----------------------------------------------------------------
class CabeEngineFixture : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        path_ = GetBenchDevice();
        if (path_.empty()) {
            state.SkipWithError("CABE_BENCH_DEVICE not set; "
                                "use scripts/mkloop.sh and `export CABE_BENCH_DEVICE=/dev/loopX`");
            return;
        }
        const cabe::Status s = cabe::Engine::Open(DefaultOptions(path_), &engine_);
        if (!s.ok()) {
            char msg[256];
            std::snprintf(msg, sizeof(msg), "cabe::Engine::Open failed: %s, path=%s",
                          s.ToString().c_str(), path_.c_str());
            state.SkipWithError(msg);
        }
    }

    void TearDown(benchmark::State&) override {
        if (engine_) {
            (void) engine_->Close();
            engine_.reset();
        }
        // 不 unlink:裸设备由调用方管理
    }

protected:
    std::unique_ptr<cabe::Engine> engine_;
    std::string                   path_;
};

// ----------------------------------------------------------------
// API_Put: 公开 API 写入开销（含 string_view→string 拷贝、span→DataView 转换、
// TranslateStatus 翻译）。与 internal BM_Engine_Put 对比即可看 API 层 overhead。
// ----------------------------------------------------------------
BENCHMARK_DEFINE_F(CabeEngineFixture, Put)(benchmark::State& state) {
    const size_t size  = static_cast<size_t>(state.range(0));
    const auto   value = MakeBytes(size);

    std::vector<std::string> keys;
    keys.reserve(kKeyPoolSize);
    for (size_t k = 0; k < kKeyPoolSize; ++k) {
        keys.push_back("k_" + std::to_string(k));
    }

    size_t i = 0;
    for (auto _ : state) {
        const cabe::Status s = engine_->Put(keys[i++ % kKeyPoolSize], value);
        if (!s.ok()) {
            state.SkipWithError("API Put failed");
            break;
        }
    }
    state.SetBytesProcessed(state.iterations() * size);
}

BENCHMARK_REGISTER_F(CabeEngineFixture, Put)
    ->Name("BM_CabeEngine_Put")
    ->Arg(1024)
    ->Arg(1024 * 1024)
    ->Arg(16 * 1024 * 1024)
    ->Unit(benchmark::kMicrosecond);

// ----------------------------------------------------------------
// API_Get: 公开 API 读取（GetIntoVector + clear 契约）开销
// ----------------------------------------------------------------
BENCHMARK_DEFINE_F(CabeEngineFixture, Get)(benchmark::State& state) {
    // SetUp SkipWithError 后 Google Benchmark 会跳过 state 循环体,但
    // **state 循环之前的预填充代码仍会执行** —— engine_ 仍是 nullptr,
    // 下面 engine_->Put 会 segfault。显式判空,直接 return。
    if (!engine_) {
        return;
    }

    const size_t size  = static_cast<size_t>(state.range(0));
    const auto   value = MakeBytes(size, 0x55);

    std::vector<std::string> keys;
    keys.reserve(kPreloadKeys);
    for (size_t k = 0; k < kPreloadKeys; ++k) {
        keys.push_back("k_" + std::to_string(k));
        if (!engine_->Put(keys[k], value).ok()) {
            state.SkipWithError("preload Put failed");
            return;
        }
    }

    std::vector<std::byte> out;
    size_t                 i = 0;
    for (auto _ : state) {
        const cabe::Status s = engine_->Get(keys[i++ % kPreloadKeys], &out);
        benchmark::DoNotOptimize(out);
        if (!s.ok()) {
            state.SkipWithError("API Get failed");
            break;
        }
    }
    state.SetBytesProcessed(state.iterations() * size);
}

BENCHMARK_REGISTER_F(CabeEngineFixture, Get)
    ->Name("BM_CabeEngine_Get")
    ->Arg(1024)
    ->Arg(1024 * 1024)
    ->Arg(16 * 1024 * 1024)
    ->Unit(benchmark::kMicrosecond);

// ----------------------------------------------------------------
// API_Delete: 公开 API 删除（含磁盘块回收）端到端开销
// ----------------------------------------------------------------
BENCHMARK_DEFINE_F(CabeEngineFixture, PutDelete)(benchmark::State& state) {
    const auto value = MakeBytes(CABE_VALUE_DATA_SIZE);

    size_t i = 0;
    for (auto _ : state) {
        const std::string key = "del_" + std::to_string(i++);
        if (!engine_->Put(key, value).ok()) {
            state.SkipWithError("Put failed in PutDelete pair");
            break;
        }
        if (!engine_->Delete(key).ok()) {
            state.SkipWithError("Delete failed in PutDelete pair");
            break;
        }
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_REGISTER_F(CabeEngineFixture, PutDelete)
    ->Name("BM_CabeEngine_PutDelete")
    ->Unit(benchmark::kMicrosecond);

// ----------------------------------------------------------------
// 并发 bench：进程级 single-instance Engine，所有线程共享
// ----------------------------------------------------------------
struct CabeConcurrentSetup {
    std::unique_ptr<cabe::Engine> engine;
    std::string                   path;
    std::vector<std::string>      keys;
    std::vector<std::byte>        value;

    // 构造失败(CABE_BENCH_DEVICE 未设置 / Open 失败 / 预填充失败)时
    // **不** abort,而是把 engine 置空 —— 让 BM_CabeEngine_ConcurrentGet
    // 在首次访问时自己判空并 SkipWithError,避免 SIGABRT 打断整个 bench 进程。
    //
    // pool 容量取 kConcurrentPoolSize(见文件顶部常量定义及注释)。
    CabeConcurrentSetup() {
        path = GetBenchDevice();
        if (path.empty()) return;

        cabe::Options opts;
        opts.device_path       = path;
        opts.buffer_pool_count = kConcurrentPoolSize;

        if (!cabe::Engine::Open(opts, &engine).ok()) {
            engine.reset();
            return;
        }

        value = MakeBytes(CABE_VALUE_DATA_SIZE, 0x77);
        keys.reserve(kPreloadKeys);
        for (size_t k = 0; k < kPreloadKeys; ++k) {
            keys.push_back("ck_" + std::to_string(k));
            if (!engine->Put(keys[k], value).ok()) {
                engine.reset();
                return;
            }
        }
    }

    ~CabeConcurrentSetup() {
        if (engine) (void) engine->Close();
        // 不 unlink:裸设备由调用方管理
    }
};

CabeConcurrentSetup& ConcurrentSetup() {
    static CabeConcurrentSetup s;
    return s;
}

void BM_CabeEngine_ConcurrentGet(benchmark::State& state) {
    auto& s = ConcurrentSetup();
    // CABE_BENCH_DEVICE 未设置 / setup 失败 → engine 为 nullptr。
    //
    // SkipWithError 在 google benchmark 里是 **per-thread** 语义:每个线程
    // 必须各自调一次,框架不会跨线程传播。如果只让 thread 0 调,其它线程
    // 既没 iterate 也没 skip,当 thread 数足够多(观察到 threads:16 触发)时
    // 框架会判定"The benchmark didn't run, nor was it explicitly skipped"。
    // 让所有线程都调 SkipWithError,框架内部会去重(看到同一条 msg)。
    if (!s.engine) {
        state.SkipWithError("CABE_BENCH_DEVICE not set or Engine setup failed; "
                            "use scripts/mkloop.sh and `export CABE_BENCH_DEVICE=/dev/loopX`");
        return;
    }

    std::vector<std::byte> out;
    size_t                 i      = static_cast<size_t>(state.thread_index());
    const size_t           stride = static_cast<size_t>(state.threads());

    for (auto _ : state) {
        const cabe::Status st = s.engine->Get(s.keys[i % kPreloadKeys], &out);
        benchmark::DoNotOptimize(out);
        if (!st.ok()) {
            state.SkipWithError("concurrent Get failed");
            break;
        }
        i += stride;
    }
    state.SetBytesProcessed(state.iterations() * CABE_VALUE_DATA_SIZE);
}

BENCHMARK(BM_CabeEngine_ConcurrentGet)
    ->ThreadRange(1, 16)
    ->Unit(benchmark::kMicrosecond);

} // namespace
