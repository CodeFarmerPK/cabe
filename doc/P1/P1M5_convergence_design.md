# Cabe P1-M5 设计：微基准基线 + P1 收敛

> 本里程碑作为 P1 阶段收敛节点：新增 Engine 级微基准（Put / Get / Delete 吞吐），归档基线
> 到 `bench/baselines/p1_single_thread.json`，撰写阶段收敛稿（本文件），并把 ROADMAP /
> README / doc/P1/ 各稿状态字串推到收敛态。完成后 P1 整体出口。
>
> **本文为详细设计**。收敛稿采用薄索引形态（与 P0M7 一致）。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P1 / M5 |
| 状态 | **完成稿（待 owner 终审）** |
| 上游依赖 | P1M1–M4 全部完成（Engine 骨架 + BufferPool + I/O + FreeList + MetaIndex + Put/Get/Delete 端到端） |
| 下游依赖本里程碑 | P1 阶段出口；P2 启动闸门 |
| 退出判定 | 见 §8（六条） |

---

## 1. 目标与范围

### 1.1 目标

1. 新增 Engine 级微基准：`bench/engine/engine_bench.cpp`（Put / Get / Delete 吞吐）。
2. 扩展 `run-bench.sh` 支持 engine bench 目标。
3. 归档基线到 `bench/baselines/p1_single_thread.json`（双工具链 × Release × 5 次重复 × 中位数）。
4. 撰写本收敛稿（薄索引形态）。
5. 状态同步：各 P1M1–M4 设计稿 + doc/P1/README.md + ROADMAP + 根 README。

### 1.2 交付范围

1. **`bench/engine/engine_bench.cpp`**：Put / Get / Delete 三个微基准。
2. **`bench/CMakeLists.txt` 修改**：注册 `bench_engine` 目标。
3. **`scripts/run-bench.sh` 修改**：遍历 `bench_*` 可执行时包含 `bench_engine`。
4. **`bench/baselines/p1_single_thread.json`**：基线归档。
5. **`doc/P1/P1M5_convergence_design.md`**：本文件。
6. **状态同步**（详见 §6）。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| P1 厚整合稿 | 未来 v1.0 发布前 | 与 P0 同策略——薄索引够用 |
| Engine 并发 bench | P7 | P1 单线程 |

---

## 2. 收敛技术索引（薄索引形态）

P1 阶段全部技术结论链回 P1M1–M4 各稿：

| 主题 | 锁定结论 | 详见 |
|---|---|---|
| Engine 公开 API | `Options` / `Status` / `Engine`（Open / Close / Put / Get / Delete）三者独立头文件 | [P1M1](P1M1_engine_skeleton_design.md) §5–§8 |
| Status 类型 | 薄包装 struct（`int code` + `ok()` + `explicit operator bool()`），4 字节 | [P1M1](P1M1_engine_skeleton_design.md) §6 |
| 返回值分层 | 公开 API 用 `Status`；内部组件（IO / FreeList / MetaIndex）用 `int32_t` 错误码；转换点唯一在 Engine 方法体内 | 项目约定 |
| Engine 状态机 | 空构造 → Open → Opened → Close → Closed；析构自动 Close | [P1M1](P1M1_engine_skeleton_design.md) §8.2 |
| BufferPool | `aligned_alloc(4096, N × 1MiB)` + LIFO；`Allocate() → byte*` / `Free(buf)` | [P1M2](P1M2_data_path_design.md) §5 |
| 朴素 I/O | `WriteBlock` / `ReadBlock`：pwrite / pread + O_DIRECT → `int32_t` | [P1M2](P1M2_data_path_design.md) §6 |
| FreeList | `vector<BlockId>` + LIFO；`Init(dev, block_count)` / `Allocate(out*) → int32_t` / `Free(id)` | [P1M3](P1M3_index_freelist_design.md) §4 |
| MetaIndex | `unordered_map<string, ValueMeta>`；`Insert` / `Lookup(key, out*) → int32_t` / `Delete` | [P1M3](P1M3_index_freelist_design.md) §5 |
| Put 路径 | 覆盖写先 Free 旧块 → Allocate 新块 → BufferPool → WriteBlock → CRC32 → MetaIndex Insert | [P1M4](P1M4_e2e_design.md) §3.1 |
| Get 路径 | MetaIndex Lookup → BufferPool → ReadBlock → CRC32 校验 → memcpy 出参 | [P1M4](P1M4_e2e_design.md) §3.2 |
| Delete 路径 | 标记删除 + 立即回收（FreeList Free + MetaIndex Delete）；不做 I/O、不发 TRIM | [P1M4](P1M4_e2e_design.md) §3.3 |
| 测试环境 | loop 设备（`scripts/mkloop.sh`）；`CABE_TEST_DEVICE` 环境变量 | [P1M2](P1M2_data_path_design.md) §8 |
| 错误码 | engine 段 8 个码（AlreadyOpen / NotOpen / InvalidOpts / InvalidValue / NotImplemented / NoSpace / PoolExhausted / DataCorrupted）+ index 段 1 个码（KeyNotFound） | `common/error_code.h` |

---

## 3. Engine 微基准设计

### 3.1 `bench/engine/engine_bench.cpp`

```cpp
// 三个微基准：BM_Put / BM_Get / BM_Delete
// 需要 CABE_TEST_DEVICE 环境变量（loop 设备）

static void BM_Put(benchmark::State& state) {
    // SetUp：Open engine + 准备 1 MiB value
    // 每次迭代：Put 一个唯一 key（循环 key 序号，写满后重开 Engine）
    // TearDown：Close engine
}

static void BM_Get(benchmark::State& state) {
    // SetUp：Open engine + 预写 N 个 key
    // 每次迭代：Get 一个已存在的 key
    // TearDown：Close engine
}

static void BM_Delete(benchmark::State& state) {
    // SetUp：Open engine + 预写 N 个 key
    // 每次迭代：Delete 一个已存在的 key（用完后重新预写）
    // TearDown：Close engine
}
```

**设计要点**：
- 需要 `CABE_TEST_DEVICE`——没设置时 bench 直接 `state.SkipWithMessage("...")`。
- Put bench 每次迭代写不同 key（避免覆盖写路径干扰）；写满后 Close + 重新 Open 重置 FreeList。
- Get bench 预写固定 key 集合，迭代时随机 / 顺序读。
- `SetBytesProcessed(state.iterations() * kValueSize)`——报告吞吐。
- `benchmark::ClobberMemory()` 防止编译器优化。

### 3.2 `bench/CMakeLists.txt` 修改

```cmake
add_executable(bench_engine engine/engine_bench.cpp)
target_link_libraries(bench_engine PRIVATE cabe::engine benchmark::benchmark_main)
```

### 3.3 `run-bench.sh` 修改

当前 `run-bench.sh` 硬编码跑 `bench_crc32` + `bench_hash`。改为**自动发现** `$build_dir/bench/bench_*` 所有可执行：

```bash
for bin in "$build_dir"/bench/bench_*; do
    [[ -x "$bin" ]] || continue
    "$bin" --benchmark_repetitions=5 ...
done
```

这样新增 `bench_engine` 无需再改脚本。

### 3.4 基线 JSON schema

复用 P0 的 schema（`p0_utilities.json` 格式），`results` 段加 engine 字段：

```json
{
  "schema_version": "1.0",
  "milestone": "P1M5",
  "results": {
    "g++": {
      "bench_crc32/64": {...},
      "bench_crc32/4096": {...},
      "bench_crc32/1048576": {...},
      "bench_hash/16": {...},
      "bench_hash/256": {...},
      "bench_hash/1048576": {...},
      "bench_engine/BM_Put": {...},
      "bench_engine/BM_Get": {...},
      "bench_engine/BM_Delete": {...}
    },
    "clang++": {...}
  }
}
```

---

## 4. 状态同步

### 4.1 各 P1M1–M4 设计稿状态字串

`doc/P1/README.md` 表格 M1–M4 行：状态列从 `待设计` 改为 `✅ 已锁定（P1M5 收敛）`。

各 P1Mn 设计稿头部 §0 元信息表"状态"行：同步改为 `✅ 已锁定（P1M5 收敛）`。

### 4.2 `doc/P1/README.md` 更新

- 状态：🚧 已启动 → ✅ 已实施
- M5 条目：`待设计` → `✅ 已锁定`
- 退出条件概要：确认 6 条全部满足

### 4.3 根 `README.md`

P1 行：`⏳ 待启动` → `✅ 完成`

### 4.4 `ROADMAP.md`

P1 段头部加 **状态**：✅ 已实施

---

## 5. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| Engine bench 需要 loop 设备 | run-bench.sh 跑 bench_engine 时如果 CABE_TEST_DEVICE 未设，bench_engine 跳过但不影响 util bench | bench_engine 内部 SkipWithMessage |
| Put bench 写满设备需重开 Engine | 每轮 Put 到 NoSpace 后 Close + Open 重置——增加 bench overhead | 用大设备（128 GiB）减少重开频率 |
| P1 基线含 I/O 延迟 | Put / Get 的数值受 loop 设备 / SSD 性能影响——不同机器差异大 | JSON schema 含 env 段记录硬件信息；基线仅作回归参考不强制不退步 |

---

## 6. 退出条件与验证步骤

### 6.1 退出条件

1. **Engine 微基准就位**：`bench/engine/engine_bench.cpp` + `bench_engine` 目标编译通过。
2. **基线归档**：`bench/baselines/p1_single_thread.json` 落盘 + `jq -e .` 校验通过。
3. **四档全绿**：`run-tests.sh --asan` / `--tsan` / `--ubsan` / `--release` 跑通（65 个用例）。
4. **覆盖率**：`run-coverage.sh --strict` ≥ 80%。
5. **状态同步全完**：各 P1Mn 稿 + doc/P1/README.md + ROADMAP + 根 README 按 §4 更新。
6. **owner 终审**：本收敛稿 + 全部改动审阅通过 → P1 整体出口。

### 6.2 验证步骤

```bash
# 创建 loop 设备
./scripts/mkloop.sh create
export CABE_TEST_DEVICE=/dev/loopN

# 四档回归
./scripts/run-tests.sh --asan
./scripts/run-tests.sh --tsan
./scripts/run-tests.sh --ubsan
./scripts/run-tests.sh --release

# 微基准 + 基线归档
./scripts/run-bench.sh --baseline=bench/baselines/p1_single_thread.json

# 覆盖率
unset CABE_TEST_DEVICE
./scripts/run-coverage.sh --strict

# JSON 校验
jq -e . bench/baselines/p1_single_thread.json

# 清理
./scripts/mkloop.sh cleanup
```

---

## 7. 对下游里程碑的接口承诺

| 下游 | 本里程碑提供的接入点 |
|---|---|
| **P2 启动** | `doc/P1/README.md` 退出条件全满足；Engine 公开 API 签名已定型（P2 冻结只调细节） |
| **P2+ 微基准回归** | `bench/baselines/p1_single_thread.json` 作为 P1 基线；`run-bench.sh` 自动发现新 bench 目标 |
| **未来 v1.0 厚整合稿** | 本收敛稿 §2 薄索引各链接可直接被厚稿展开 |

---

**全文完。**
