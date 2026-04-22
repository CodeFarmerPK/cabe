/*
 * ChunkIndex micro-benchmark.
 *
 * 覆盖 P2 Engine 调用 ChunkIndex 的全部 hot paths：
 *   - GetRange    （Get 路径，1/16/1024 chunks）
 *   - Put         （Put 路径，每 chunk 一次）
 *   - DeleteRange （内部 Delete 路径，标记删除）
 *   - RemoveRange （Remove 路径，物理移除 + 回收）
 *
 * std::map 的 lower_bound + erase(it++) 走 O(N log M) 扫描，bench 量化
 * 之后能直接对比 P6 自研 B+ 树替换后的提升。
 */
#include "memory/chunk_index.h"
#include <benchmark/benchmark.h>
#include <vector>

namespace {

    constexpr uint32_t kMapSize = 100'000;

    ChunkMeta MakeMeta(uint64_t i) {
        return ChunkMeta{
            .blockId = i,
            .crc = 0,
            .timestamp = 0,
            .state = DataState::Active,
        };
    }

    // 把 idx 填充到 kMapSize（覆盖式 Put：无论原状态如何，最终全为 Active）。
    //
    // 注意：ChunkIndex 显式 delete 了 move ctor / move assign（P1.2 设计——
    // 持状态资源不可移动），不能用 `return ChunkIndex` 工厂函数。改用输出参数。
    // 重填路径利用 std::map::operator[]= 的覆盖语义，无需先 RemoveRange。
    void Populate(ChunkIndex& idx) {
        for (uint32_t i = 0; i < kMapSize; ++i) {
            idx.Put(i, MakeMeta(i));
        }
    }
    // ----------------------------------------------------------------
    // GetRange: 读路径（已有）
    // ----------------------------------------------------------------
    void BM_ChunkIndex_GetRange(benchmark::State& state) {
        const auto count = static_cast<uint32_t>(state.range(0));

        ChunkIndex idx;
        Populate(idx);
        std::vector<ChunkMeta> out;
        out.reserve(count);

        uint32_t start = 0;
        const uint32_t step = count;
        for (auto _ : state) {
            idx.GetRange(start, count, &out);
            benchmark::DoNotOptimize(out);
            start += step;
            if (start + count > kMapSize) {
                start = 0;
            }
        }
        state.SetItemsProcessed(state.iterations() * count);
    }

    // ----------------------------------------------------------------
    // Put: 单 chunk 插入。Engine::Put 每 chunk 一次。
    // 在已填满 kMapSize 的 map 上做覆盖写（key 复用），稳态成本。
    // ----------------------------------------------------------------
    void BM_ChunkIndex_Put(benchmark::State& state) {
        ChunkIndex idx;
        Populate(idx);

        ChunkId i = 0;
        for (auto _ : state) {
            idx.Put(i % kMapSize, MakeMeta(i));
            ++i;
        }
        state.SetItemsProcessed(state.iterations());
    }

    // ----------------------------------------------------------------
    // DeleteRange: 内部 Delete 路径（仅 mark Deleted，不物理移除）
    // 两遍扫描：先验证整段命中，再批量 mutation
    // ----------------------------------------------------------------
    void BM_ChunkIndex_DeleteRange(benchmark::State& state) {
        const auto count = static_cast<uint32_t>(state.range(0));

        ChunkIndex idx;
        Populate(idx);

        // 每个 iter 切换到下一段，跑完一轮后重新 Active（避免后续 iter 全部
        // 命中 Deleted 状态导致路径退化）
        uint32_t start = 0;
        for (auto _ : state) {
            idx.DeleteRange(start, count);
            start += count;
            if (start + count > kMapSize) {
                state.PauseTiming();
                // 重新填充：用新的 Active 覆盖刚 Deleted 的状态
                Populate(idx);
                start = 0;
                state.ResumeTiming();
            }
        }
        state.SetItemsProcessed(state.iterations() * count);
    }

    // ----------------------------------------------------------------
    // RemoveRange: Remove 路径（物理 erase）。Engine::Remove 的核心成本。
    //
    // 关键：避免 PauseTiming + 100K Put 重建的 setup 成本主导测量。
    //   做法：预填充一次 kMapSize，从 cursor=0 开始按 count 步长连续 RemoveRange，
    //         直到 map 即将耗尽时才 PauseTiming 重建。
    //   摊销：count=1 时 100K iter 不重建；count=16 时 ~6250 iter 重建一次；
    //         count=1024 时 ~97 iter 重建一次。重建成本被均摊到每段 iter 上，
    //         绝大多数 iter 都是纯 RemoveRange 信号。
    // ----------------------------------------------------------------
    void BM_ChunkIndex_RemoveRange(benchmark::State& state) {
        const auto count = static_cast<uint32_t>(state.range(0));

        ChunkIndex idx;
        Populate(idx);
        uint32_t cursor = 0;
        for (auto _ : state) {
            if (cursor + count > kMapSize) {
                // map 即将耗尽，Populate 把被 erase 的 chunk 重新 Put 回去
                state.PauseTiming();
                Populate(idx);
                cursor = 0;
                state.ResumeTiming();
            }
            idx.RemoveRange(cursor, count);
            cursor += count;
        }
        state.SetItemsProcessed(state.iterations() * count);
    }

} // namespace

BENCHMARK(BM_ChunkIndex_GetRange)->Arg(1)->Arg(16)->Arg(1024)->Unit(benchmark::kNanosecond);

BENCHMARK(BM_ChunkIndex_Put)->Unit(benchmark::kNanosecond);

BENCHMARK(BM_ChunkIndex_DeleteRange)->Arg(1)->Arg(16)->Arg(1024)->Unit(benchmark::kNanosecond);

BENCHMARK(BM_ChunkIndex_RemoveRange)->Arg(1)->Arg(16)->Arg(1024)->Unit(benchmark::kMicrosecond);
