---
name: Cabe 项目路线图与状态
description: Cabe KV 存储引擎各阶段完成情况、架构决策和关键约束
type: project
---

## 完成状态（截至 2026-04-27）

- **P-1** Fedora 43 开发环境 — ✅ 已完成
- **P0** BufferPool（mmap + O_DIRECT 对齐） — ✅ 已完成
- **P1.1** Google Benchmark 骨架 — ✅ 已完成
- **P1.2** 线程安全（shared_mutex + atomic） — ✅ 已完成
- **P2** C++ API 契约定型 — ✅ 已完成（cabe::Engine + Status + Options，Pimpl 隔离）
- **P3** IoBackend 抽象层（编译期 dispatch，仅 sync 后端） — 🚧 下一步
  - M1 `io/` 目录骨架 + PIMPL + concept 强校验
  - M2 SyncIoBackend 填充实现（迁移 `storage/` + `buffer/` 逻辑）
  - M3 Engine 切换到 IoBackend（含 Q2 尾部补 memset）
  - M4 CMake `CABE_IO_BACKEND` 选项 + CMakePresets + `scripts/run-tests.sh --backend=`
  - M5 删除旧 `storage/storage.*` 与 `buffer/buffer_pool.*`
  - M6 文档更新 + bench 基线归档（`p3-post-io-abstraction`）
- **P4** io_uring 后端 + registered buffer pool（io_uring 接管 BufferPool） — 计划中
- **P5** WAL + 崩溃恢复 — 计划中
- **P6** 多线程 Reactor 引擎 — 计划中
- **P7** 自研 B+ 树 + 细粒度并发 — 计划中
- **P8** scatter-gather 多 chunk 合并 I/O — 计划中
- **P9** SPDK 用户态驱动后端（可选） — 计划中
## P2 实现摘要（C++ API，非 C API）

经过设计讨论后改用纯 C++ API。理由：单进程嵌入式定位 + Fedora 43 单平台，
不需要 C ABI 跨语言；C++ API 可以用 RAII / Status / Pimpl 给出更安全的契约。

新增公开头文件（`include/cabe/`）：
- `status.h` — `class [[nodiscard]] Status` 七种错误分类
- `options.h` — `Options` / `ReadOptions` / `WriteOptions`
- `engine.h` — `class Engine`，Pimpl 隔离内部 `::Engine`
- `cabe.h` — 上述三个的 umbrella include

新增实现：
- `engine/engine_api.cpp` — Pimpl 实现 + `TranslateStatus` 内部错误码翻译
- `engine/engine.h/.cpp` — 新增 `GetIntoVector(key, vector<byte>*)`，单次 shared_lock 内消除 TOCTOU
- `test/engine/engine_api_test.cpp` — 单线程功能测试
- `test/engine/cabe_engine_thread_test.cpp` — 公开 API 并发测试

CMake 配置：
- `target_include_directories(cabe_lib PUBLIC include/ PRIVATE source_root)`
  下游只能 `#include "cabe/..."`，无法绕过 Pimpl 访问内部头
## P1.2 实现摘要

共修改 5 个文件：
1. `buffer/buffer_pool.h/.cpp` — 新增 `mutable std::mutex stackMutex_` 保护 `freeStack_`；`memset` 在锁外执行
2. `engine/engine.h` — 新增 `mutable std::shared_mutex mutex_`；`nextChunkId_` 改为 `std::atomic<ChunkId>`
3. `engine/engine.cpp` — Put/Delete/Remove/Open/Close 用 `unique_lock`；Get/Size/IsOpen 用 `shared_lock`；析构函数委托给 `Close()`；`AllocateChunkIds` 改用 `fetch_add(relaxed)`
4. 新增 `test/engine/engine_thread_test.cpp` — 9 个并发测试用例（含 P3 前补充的 Delete+Get、跨 key Put+Delete 场景）
5. `CMakeLists.txt` — 注册新测试文件

## P1.1 实现摘要

`bench/` 下 7 个 Google Benchmark 文件：
- 6 个组件级（buffer_pool / free_list / meta_index / chunk_index / crc32 / engine 内部）
- 1 个 P2 公开 API（cabe_engine_bench，含并发用例）
- engine + cabe_engine bench 均含 ConcurrentGet/Put/MixedRW（ThreadRange 1..16）
- baselines/ 保留 P1.1 / P1.2 / P2 三阶段 JSON 用于回归对比

## P3 启动前已完成的清理

- TranslateStatus 错误码完整覆盖（CHUNK_NOT_FOUND/CHUNK_DELETED → Corruption；MEMORY_EMPTY_KEY/VALUE 补全）
- `GetIntoVector` 失败时 `*out` 清空契约统一（前置 clear）
- `cabe::Engine::Open` 内部初始化失败后 unlink 已创建文件
- `Put`/`Get`/`Delete` 加 `impl_` 空指针保护
- `Put({},k,v)` 噪音消除：加 `Put(k,v)` 等便捷重载
- CMake PUBLIC 内部头泄漏修复（PRIVATE source_root）
- `~Engine` 并发约束、单进程多实例约束 文档化到 `include/cabe/engine.h`
- `engine.h` 加 P3 多 NVMe 拆分 TODO，给出 per-device 锁路线
- bench：engine_bench 容量配置修复（kKeyPoolSize 4096，避免磁盘耗尽）；
  free_list / chunk_index / meta_index / buffer_pool 各加并发 / 缺失路径覆盖

## 架构要点

- **两层索引**：MetaIndex（key→KeyMeta，unordered_map）+ ChunkIndex（ChunkId→ChunkMeta，map）
- **Chunk 大小**：固定 1 MiB（CABE_VALUE_DATA_SIZE）
- **ChunkId**：全局原子自增；通过策略映射到各设备的 BlockId（未来多 NVMe）
- **多 NVMe 规划**：轮转分配 ChunkId→DeviceIndex；每设备独立 FreeList + IoBackend（P4+）
- **锁演进路径**：P1.2 粗粒度锁 → P4 io_uring I/O 脱锁 → P6 per-key 条带锁 → P7 无锁

- **最终目标**：无锁并发，充分利用多 NVMe + 多核/多 Socket 直至硬件瓶颈

## P3 关键工作项（IoBackend 抽象）

新 P3 范围：仅做 sync 后端 + 编译期 dispatch 抽象层，**不接入 io_uring**。
M1–M6 步骤见上方 P3 列表。每步必须 test 全绿才进入下一步；M5 是破坏性
改动（删除 `storage/` `buffer/` 旧目录），前置条件是 M3 全部 engine_test 通过。

### Q1–Q7 决策（已闭环）

| # | 主题 | 决定 |
|---|---|---|
| Q1 | BufferHandle 归还 | RAII 析构自动归还 |
| Q2 | AcquireBuffer 清零 | 改契约："内容未定义"；Engine::Put 小 value 分支补尾 memset |
| Q3 | 池耗尽 | 立刻失败（invalid handle / `IO_BACKEND_POOL_EXHAUSTED`） |
| Q4 | `buffer_pool_count` 语义 | 保持 slot 语义（`cabe::Options` 公开 API 不变） |
| Q5 | backend 选择机制 | 宏 `CABE_IO_BACKEND_SYNC=1` / `_IO_URING=1` / `_SPDK=1` |
| Q6 | SPDK 路线 | 保留为 P9 |
| Q7 | Close + 未归还 handle | Debug abort / Release 警告 + 强制释放 |

**衍生约束：** Q2=A 是零拷贝路线（P4 FIXED / P8 scatter-gather / P9 SPDK）的前置契约，
不能在后续阶段反复变更。

### 移出 P3 的工作项

原 P3 计划中以下工作项已移出 P3，各归各位：

1. **io_uring 集成 + I/O 脱锁** → **P4**（io_uring 后端实现期）
2. **多 NVMe 拆分**（`freeList_` / `storage_` → vector）→ 跨阶段架构，**P4+** 起
   逐步落地；每设备一个 IoBackend 实例 + per-device mutex
3. **LOCK 文件**（`Engine::Open` 加 `flock`）→ 独立工作项，可在 P3/P4 任意时间
   穿插，不阻塞 IoBackend 抽象
4. **Get 输出参数演进**（`std::expected` / zero-copy span）→ 与外部零拷贝 API
   一起，纳入 **P7+** 公开 API 演进
5. **公开 API 端到端 bench** → cabe_engine_bench 已就位，P3 M6 用作回归基线；
   P4 io_uring 启用后立即对比

## 关键约束

- 仅支持 Linux（Fedora 43+），O_DIRECT + O_SYNC
- C++20（GCC 15+ 或 Clang 20+）
- 单机存储，无分布式需求
- TSAN 支持：`cmake -DCABE_ENABLE_TSAN=ON`

**原因：** 用户目标是推到多 NVMe 硬件瓶颈，最终迁移至 SPDK。
**应用方式：** 所有设计决策须保留无锁/异步演进路径，不能硬编码单设备假设。
