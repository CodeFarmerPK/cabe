/*
* FreeList micro-benchmark.
 *
 * 三个场景，覆盖真实工作负载的不同阶段：
 *   1. Allocate-only：纯写入路径（永不 Release），走 nextBlockId++
 *   2. Allocate + Release 1:1：稳态 GC 后的 churn，刚 Release 立刻 Allocate
 *      仍走 nextBlockId++（free stack 来不及堆积）
 *   3. PreFilledStack Allocate：预先 Release 大量 block，让后续 Allocate
 *      命中 free stack 的 pop_back 路径——这才是 Delete 后再 Put 的真实路径
 *
 * 关于线程安全：FreeList 自身无 mutex，由 Engine 的 unique_lock 串行化。
 * 因此本 bench 只测算法成本，不测锁开销（锁开销在 engine_bench 并发用例里）。
 */
#include "storage/free_list.h"

#include <benchmark/benchmark.h>
#include <vector>

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
    // 预先填充 N 个空闲 block，再纯 Allocate，确保走 free stack 的 pop_back
    // 而不是 nextBlockId++。模拟"大批 Delete 之后 Put 复用 chunk"的稳态。
    // N 用 state.range(0)：覆盖小池 (256) 和大池 (16K) 两个量级。
    void BM_FreeList_AllocateFromStack(benchmark::State& state) {
        const auto poolSize = static_cast<size_t>(state.range(0));
        FreeList   fl;
        BlockId    tmp = 0;

        // 预填：先 Allocate poolSize 个，然后全部 Release，让 free stack 满
        for (size_t i = 0; i < poolSize; ++i) {
            fl.Allocate(&tmp);
        }
        for (BlockId id = 0; id < poolSize; ++id) {
            fl.Release(id);
        }

        // 计时循环：Allocate 后立即 Release，让 stack 维持非空
        // （否则 N 次 iter 后 stack 空，又退回 nextBlockId++ 路径）
        BlockId id = 0;
        for (auto _ : state) {
            fl.Allocate(&id);
            benchmark::DoNotOptimize(id);
            fl.Release(id);
        }
        state.SetItemsProcessed(state.iterations());
    }

    // 批量回收路径（Engine::Remove 用 ReleaseBatch）
    // batch_size 通过 state.range(0)：1 / 16 / 1024 chunks
    void BM_FreeList_ReleaseBatch(benchmark::State& state) {
        const auto batchSize = static_cast<size_t>(state.range(0));
        FreeList   fl;
        BlockId    tmp = 0;
        // 预先分配，留出可被 batch release 的 blockId 序列
        // 每次 iter 释放一个 fresh batch，Allocate 出新的；状态机随时间漂移
        // 但 ReleaseBatch 复杂度对 batchSize 是 O(N²)（双重重复检测），
        // 关注点是这条 hot path 的绝对耗时
        for (auto _ : state) {
            state.PauseTiming();
            std::vector<BlockId> batch;
            batch.reserve(batchSize);
            for (size_t i = 0; i < batchSize; ++i) {
                fl.Allocate(&tmp);
                batch.push_back(tmp);
            }
            state.ResumeTiming();

            fl.ReleaseBatch(batch);
        }
        state.SetItemsProcessed(state.iterations() * batchSize);
    }

} // namespace

BENCHMARK(BM_FreeList_Allocate)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FreeList_AllocateRelease)->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FreeList_AllocateFromStack)
    ->Arg(256)
    ->Arg(16 * 1024)
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_FreeList_ReleaseBatch)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1024)
    ->Unit(benchmark::kNanosecond);