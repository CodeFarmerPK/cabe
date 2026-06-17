// P6M3：多线程 Wal 吞吐微基准——测 group commit 的并发收益。
// 设计依据：doc/P6/P6M3_convergence_design.md §4。
//
// 直打 Wal（不经 Engine——提交组的并发只在 Wal 模块层存在，Engine 接口单线程从它测不出
//   并发收益）；google-benchmark `->Threads(1/2/4/8)`，框架起 N 线程跑循环、聚合 ops/墙钟。
// 共享 Wal 开一次：thread_index()==0 在 SetUp 开 fresh 环（同步档级别 3），靠框架计时屏障
//   保证开一次 + 可见性（所有线程 SetUp 跑完才过屏障，屏障提供 happens-before）。
// 度量：聚合 frames/sec（SetItemsProcessed 跨线程求和）；× 1MiB = 等效 value 提交吞吐。
//   主看 1/2/4/8 写者吞吐曲线（1 写者退化自任 leader 无合并 → 多写者 fsync 摊薄、吞吐递增）。
//   只测吞吐、不测合并率（合并率需 Wal 暴露计数器 = 改产品代码，推迟到发布后）。
// 容量：直打 Wal 无回收（无 Engine/快照来 ReclaimUpTo），WAL 环只进不出。靠 bench WAL 设备
//   1 GiB（≈8M 帧）+ 每配置 fresh 环 + 运行时长有界（默认 min-time）三管齐下防写满。
// 设备：读 CABE_TEST_WAL_DEVICE（M4 用 mkloop create-bench 的 bench WAL 设备 1G）。

#include "wal/wal.h"
#include "engine/options.h"
#include "common/error_code.h"
#include "common/structs.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

std::string GetEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : "";
}

class WalConcurrencyBench : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        // 仅 0 号线程开 fresh 环——框架计时屏障保证其余线程过屏障前 Wal 已开且可见。
        if (state.thread_index() == 0) {
            wal_path_ = GetEnv("CABE_TEST_WAL_DEVICE");
            if (wal_path_.empty()) {
                state.SkipWithMessage("需要 CABE_TEST_WAL_DEVICE（建议用 mkloop create-bench 的 1G WAL）");
                return;
            }
            opts_.create    = true;                          // 每配置 fresh 空环
            opts_.wal_level = cabe::WalLevel::WalSync;        // 同步档（级别 3）：提交组在此生效
            opts_.snapshot_threshold_bytes = 1024 * 1024;    // 仅过 Open 容量校验（直打 Wal 不快照）
            const int32_t rc = wal_.Open(wal_path_, &opts_);
            if (rc != cabe::err::kSuccess) {
                state.SkipWithMessage("Wal::Open 失败（检查 bench WAL 设备容量 ≥ 1G）");
                return;
            }
        }
    }

    void TearDown(benchmark::State& state) override {
        if (state.thread_index() == 0) {
            wal_.Close();
        }
    }

protected:
    cabe::Wal     wal_;        // 跨线程共享（夹具单实例）
    cabe::Options opts_;
    std::string   wal_path_;
};

} // namespace

BENCHMARK_DEFINE_F(WalConcurrencyBench, BM_WalCommit)(benchmark::State& state) {
    // 极简载荷——吞吐不在乎内容；每线程一个固定 entry（block 用 thread_index 区分，纯防雷同）。
    cabe::WalEntry e{};
    e.type      = cabe::WalEntryType::Put;
    e.key       = "k";                                       // 常量 key（字面量静态生命周期）
    e.block     = cabe::BlockId::Make(0, static_cast<std::uint64_t>(state.thread_index()));
    e.value_crc = 1;
    e.timestamp = 1;

    for (auto _ : state) {
        const int32_t rc = wal_.WriteWal(e);
        if (rc != cabe::err::kSuccess) {
            // 环写满（无回收）——说明运行时长过长 / WAL 设备太小。提示后停。
            state.SkipWithMessage("WriteWal 失败（环写满？用 1G bench WAL + 缩短 min-time）");
            break;
        }
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());             // 框架跨线程求和 → 聚合 frames/sec
}
// 聚合吞吐 vs 写者数（1/2/4/8）——group commit 成绩单。frames/sec × 1MiB = 等效 value 吞吐。
// UseRealTime：多线程吞吐按墙钟算（CPU 时间会跨线程累加、失真）。
BENCHMARK_REGISTER_F(WalConcurrencyBench, BM_WalCommit)
    ->Unit(benchmark::kMicrosecond)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)
    ->UseRealTime();
