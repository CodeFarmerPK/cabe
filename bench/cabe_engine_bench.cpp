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
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint64_t kBackingFileBytes = 8ULL * 1024 * 1024 * 1024; // 8 GiB
constexpr size_t   kKeyPoolSize      = 4096;                       // 同 engine_bench：避免磁盘耗尽
constexpr size_t   kPreloadKeys      = 256;                        // 并发 bench 共享 key 池

bool SupportsDirectIO(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    if (::ftruncate(fd, static_cast<off_t>(kBackingFileBytes)) < 0) {
        ::close(fd);
        ::unlink(path.c_str());
        return false;
    }
    ::close(fd);
    fd = ::open(path.c_str(), O_RDWR | O_DIRECT | O_SYNC);
    if (fd < 0) {
        ::unlink(path.c_str());
        return false;
    }
    ::close(fd);
    ::unlink(path.c_str()); // cabe::Engine::Open 会重新创建
    return true;
}

cabe::Options DefaultOptions(const std::string& path) {
    cabe::Options opts;
    opts.path              = path;
    opts.create_if_missing = true;
    opts.error_if_exists   = false;
    opts.buffer_pool_count = 8;
    opts.initial_file_size = kBackingFileBytes;
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
// Fixture：每个 benchmark 用独立 backing file 与 cabe::Engine 实例
// ----------------------------------------------------------------
class CabeEngineFixture : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        std::string name = state.name();
        std::replace(name.begin(), name.end(), '/', '_');
        path_ = "/var/tmp/cabe_api_bench_" +
                std::to_string(::getpid()) + "_" + name + ".dat";

        if (!SupportsDirectIO(path_)) {
            state.SkipWithError("O_DIRECT not supported on hosting filesystem");
            return;
        }
        const cabe::Status s = cabe::Engine::Open(DefaultOptions(path_), &engine_);
        if (!s.ok()) {
            ::unlink(path_.c_str());
            state.SkipWithError("cabe::Engine::Open failed");
            return;
        }
    }

    void TearDown(benchmark::State&) override {
        if (engine_) {
            (void) engine_->Close();
            engine_.reset();
        }
        ::unlink(path_.c_str());
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

    CabeConcurrentSetup() {
        path = "/var/tmp/cabe_api_bench_concurrent_" + std::to_string(::getpid()) + ".dat";
        if (!SupportsDirectIO(path)) std::abort();

        const cabe::Status s = cabe::Engine::Open(DefaultOptions(path), &engine);
        if (!s.ok()) {
            ::unlink(path.c_str());
            std::abort();
        }

        value = MakeBytes(CABE_VALUE_DATA_SIZE, 0x77);
        keys.reserve(kPreloadKeys);
        for (size_t k = 0; k < kPreloadKeys; ++k) {
            keys.push_back("ck_" + std::to_string(k));
            if (!engine->Put(keys[k], value).ok()) std::abort();
        }
    }

    ~CabeConcurrentSetup() {
        if (engine) (void) engine->Close();
        ::unlink(path.c_str());
    }
};

CabeConcurrentSetup& ConcurrentSetup() {
    static CabeConcurrentSetup s;
    return s;
}

void BM_CabeEngine_ConcurrentGet(benchmark::State& state) {
    auto&                  s = ConcurrentSetup();
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
