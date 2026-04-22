/*
* BufferPool micro-benchmark.
 *
 * 测量内容：
 *   1. 单线程 Acquire+Release round-trip（含 1 MiB memset，~50μs 主导）
 *   2. 多线程并发 Acquire+Release，量化 stackMutex_ 在 P1.2 shared_lock
 *      场景下的隐藏争用
 *
 *P1.2 引入 BufferPool 自身 mutex（独立于 Engine mutex_）后，
 * 多 reader 并发 Get 时所有 Acquire/Release 仍在 stackMutex_ 上排队。
 * memset 在锁外执行，临界区只有 pop_back/push_back（~几 ns），但 N 线程
 * 高频争用时 cache line ping-pong 仍然可观察。这条 bench 给 P3+ 替换
 * 为 lock-free / per-thread free list 时提供 baseline。
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

    // 并发 bench：所有线程共享一个 BufferPool，验证 stackMutex_ 争用成本。
    // 池容量 = kBufCount(64) > 最大 ThreadRange(16)，保证不会因池耗尽返回
    // nullptr 把 bench 数据污染掉。
    //
    // 注意：BufferPool 显式 delete 了 move/copy（持 mmap 区不可移动），
    // 不能用 lambda 返回 BufferPool 的方式初始化静态变量。改用 holder 结构
    // 在构造期就 Init，靠 C++11 函数静态局部的线程安全初始化保证一次性建立。
    BufferPool& SharedPool() {
        struct Holder {
            BufferPool pool;
            Holder() {
                if (pool.Init(kBufSize, kBufCount) != SUCCESS) {
                    std::abort();
                }
            }
        };
        static Holder h;
        return h.pool;
    }

    void BM_BufferPool_ConcurrentAcquireRelease(benchmark::State& state) {
        BufferPool& pool = SharedPool();
        for (auto _ : state) {
            char* p = pool.Acquire();
            benchmark::DoNotOptimize(p);
            if (p == nullptr) {
                state.SkipWithError("pool exhausted");
                break;
            }
            pool.Release(p);
        }
        state.SetBytesProcessed(state.iterations() * kBufSize);
    }
} // namespace

BENCHMARK(BM_BufferPool_AcquireRelease)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_BufferPool_ConcurrentAcquireRelease)
    ->ThreadRange(1, 16)
    ->Unit(benchmark::kMicrosecond);