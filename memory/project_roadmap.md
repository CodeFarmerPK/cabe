---
name: Cabe 项目路线图与状态
description: Cabe KV 存储引擎各阶段完成情况、架构决策和关键约束
type: project
---

## 完成状态（截至 2026-04-21）

- **P-1** Fedora 43 开发环境 — ✅ 已完成
- **P0** BufferPool（mmap + O_DIRECT 对齐） — ✅ 已完成
- **P1.1** Google Benchmark 骨架 — ✅ 已完成
- **P1.2** 线程安全（shared_mutex + atomic） — ✅ 已完成（本次会话实现）
- **P2** C API 契约定型 — 计划中
- **P3** io_uring 异步 I/O + 异步 API — 计划中
- **P4** WAL + 崩溃恢复 — 计划中
- **P5** 多线程 Reactor 引擎 — 计划中
- **P6** 自研 B+ 树 + 细粒度并发 — 计划中
- **P7** 零拷贝（registered buffers / scatter-gather） — 计划中
- **P8** SPDK 用户态驱动（可选） — 计划中

## P1.2 实现摘要

共修改 5 个文件：
1. `buffer/buffer_pool.h/.cpp` — 新增 `mutable std::mutex stackMutex_` 保护 `freeStack_`；`memset` 在锁外执行
2. `engine/engine.h` — 新增 `mutable std::shared_mutex mutex_`；`nextChunkId_` 改为 `std::atomic<ChunkId>`
3. `engine/engine.cpp` — Put/Delete/Remove/Open/Close 用 `unique_lock`；Get/Size/IsOpen 用 `shared_lock`；析构函数委托给 `Close()`；`AllocateChunkIds` 改用 `fetch_add(relaxed)`
4. 新增 `test/engine/engine_thread_test.cpp` — 7 个并发测试用例
5. `CMakeLists.txt` — 注册新测试文件

## 架构要点

- **两层索引**：MetaIndex（key→KeyMeta，unordered_map）+ ChunkIndex（ChunkId→ChunkMeta，map）
- **Chunk 大小**：固定 1 MiB（CABE_VALUE_DATA_SIZE）
- **ChunkId**：全局原子自增；通过策略映射到各设备的 BlockId（未来多 NVMe）
- **多 NVMe 规划**：轮转分配 ChunkId→DeviceIndex；每设备独立 FreeList + Storage（P3+）
- **锁演进路径**：P1.2 粗粒度锁 → P3 I/O 脱锁 → P5 per-key 条带锁 → P6 无锁
- **最终目标**：无锁并发，充分利用多 NVMe + 多核/多 Socket 直至硬件瓶颈

## 关键约束

- 仅支持 Linux（Fedora 43+），O_DIRECT + O_SYNC
- C++20（GCC 15+ 或 Clang 20+）
- 单机存储，无分布式需求
- TSAN 支持：`cmake -DCABE_ENABLE_TSAN=ON`

**原因：** 用户目标是推到多 NVMe 硬件瓶颈，最终迁移至 SPDK。
**应用方式：** 所有设计决策须保留无锁/异步演进路径，不能硬编码单设备假设。
