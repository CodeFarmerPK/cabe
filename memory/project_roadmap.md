---
name: Cabe 项目路线图与状态
description: Cabe KV 存储引擎各阶段完成情况、架构决策和关键约束
type: project
---

## 完成状态（截至 2026-04-22）

- **P-1** Fedora 43 开发环境 — ✅ 已完成
- **P0** BufferPool（mmap + O_DIRECT 对齐） — ✅ 已完成
- **P1.1** Google Benchmark 骨架 — ✅ 已完成
- **P1.2** 线程安全（shared_mutex + atomic） — ✅ 已完成
- **P2** C++ API 契约定型 — ✅ 已完成（cabe::Engine + Status + Options，Pimpl 隔离）
- **P3** io_uring 异步 I/O + 异步 API + LOCK 文件 — 进行中
- **P4** WAL + 崩溃恢复 — 计划中
- **P5** 多线程 Reactor 引擎 — 计划中
- **P6** 自研 B+ 树 + 细粒度并发 — 计划中
- **P7** 零拷贝（registered buffers / scatter-gather） — 计划中
- **P8** SPDK 用户态驱动（可选） — 计划中

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
- **多 NVMe 规划**：轮转分配 ChunkId→DeviceIndex；每设备独立 FreeList + Storage（P3+）
- **锁演进路径**：P1.2 粗粒度锁 → P3 I/O 脱锁 → P5 per-key 条带锁 → P6 无锁
- **最终目标**：无锁并发，充分利用多 NVMe + 多核/多 Socket 直至硬件瓶颈

## P3 关键工作项（启动后）

1. **多 NVMe 拆分**（engine.h TODO）：`freeList_` / `storage_` → vector，per-device mutex
2. **io_uring 集成**：Put/Get 的 I/O 阶段脱离 Engine mutex_
3. **LOCK 文件**：`Engine::Open` 加 `flock`，多进程互斥同 path
4. **Get 输出参数演进**：考虑 `std::expected<vector<byte>, Status>` 或 zero-copy span
5. **公开 API 端到端 bench**：cabe_engine_bench 已就位，io_uring 启用后立即对比

## 关键约束

- 仅支持 Linux（Fedora 43+），O_DIRECT + O_SYNC
- C++20（GCC 15+ 或 Clang 20+）
- 单机存储，无分布式需求
- TSAN 支持：`cmake -DCABE_ENABLE_TSAN=ON`

**原因：** 用户目标是推到多 NVMe 硬件瓶颈，最终迁移至 SPDK。
**应用方式：** 所有设计决策须保留无锁/异步演进路径，不能硬编码单设备假设。
