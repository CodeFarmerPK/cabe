---
name: Cabe 项目路线图与状态
description: Cabe KV 存储引擎各阶段完成情况、架构决策和关键约束
type: project
originSessionId: 5e75530e-aa98-44be-b0dc-01b60f844823
---

> **同步说明**:此文件与项目目录 `cabe/memory/project_roadmap.md` 保持一致。
> 项目目录版本是正本(README、设计文档引用它),此处镜像供 Claude 会话自动读入。
> 最近一次同步:2026-05-14(P4 M4 完成后)。

## 完成状态（截至 2026-05-14）

- **P-1** Fedora 43 开发环境 — ✅ 已完成
- **P0** BufferPool（mmap + O_DIRECT 对齐） — ✅ 已完成
- **P1.1** Google Benchmark 骨架 — ✅ 已完成
- **P1.2** 线程安全（shared_mutex + atomic） — ✅ 已完成
- **P2** C++ API 契约定型 — ✅ 已完成（cabe::Engine + Status + Options，Pimpl 隔离）
- **P3** IoBackend 抽象层（编译期 dispatch，仅 sync 后端） — ✅ 已完成
  - M1 `io/` 目录骨架 + PIMPL + concept 强校验
  - M2 SyncIoBackend 填充实现（迁移 `storage/` + `buffer/` 逻辑）
  - M3 Engine 切换到 IoBackend（含 Q2 尾部补 memset）
  - M4 CMake `CABE_IO_BACKEND` 选项 + CMakePresets + `scripts/run-tests.sh --backend=`
  - M5 删除旧 `storage/storage.*` 与 `buffer/buffer_pool.*`
  - M6 文档更新 + bench 基线归档（`p3-post-io-abstraction`）
- **P4** io_uring 后端 + registered buffer pool（io_uring 接管 BufferPool） — 🚧 实施中（M1-M5 完成,M6 待启动）
  - 设计稿:`doc/p4_io_uring_design.md`(2026-04-28 v1.0)
  - 进度:9 个 milestone 中 5 个完成(M1 骨架+TSAN阻断 / M2 Open-Close / M3 W-R /
    M4 register_buffers+FIXED / M5 register_files+IOSQE_FIXED_FILE)
  - bench 验证:M4 完成 cpu_time 加速 16-82%(详见 `bench/baselines/p4-pre-fixed-*.json`
    与 `p4-post-fixed-*.json`);M5 完成待跑 bench 归档 `p4-post-fixed-files`
    (预期再 +1-3% cpu_time,fdget/fdput 摊销)
  - 决策:19 项中 17 项落地;D7 第二部分(Options.io_uring_sq_depth)与 D10
    (CABE_HAVE_* feature gate)待 M6 闭环
  - 风险:12 项中 11 项闭环;R12(sq_depth >= pool_count 校验)待 M6 引入 Options 字段一并加
  - 注释/文档同步状态:截至 M5 已统一刷到 M5 标号
  - 详见下文「## P4 实施计划」(各 milestone 已加 ✅/❌ 状态)
- **P5** WAL + 崩溃恢复 — 计划中
- **P6** 多线程 Reactor 引擎 — 计划中
- **P7** 自研 B+ 树 + 细粒度并发 — 计划中
- **P8** scatter-gather 多 chunk 合并 I/O — 计划中
- **P9** SPDK 用户态驱动后端（可选） — 计划中

## 架构要点

- **两层索引**：MetaIndex（key→KeyMeta，unordered_map）+ ChunkIndex（ChunkId→ChunkMeta，map）
- **Chunk 大小**：固定 1 MiB（CABE_VALUE_DATA_SIZE）
- **ChunkId**：全局原子自增；通过策略映射到各设备的 BlockId（未来多 NVMe）
- **多 NVMe 规划**：轮转分配 ChunkId→DeviceIndex；每设备独立 FreeList + IoBackend（P4+）
- **锁演进路径**：P1.2 粗粒度锁 → P4 io_uring I/O 脱锁 → P6 per-key 条带锁 → P7 无锁
- **最终目标**：无锁并发，充分利用多 NVMe + 多核/多 Socket 直至硬件瓶颈

## P3 实现摘要（IoBackend 抽象）

P3 范围：sync 后端 + 编译期 dispatch 抽象层（不含 io_uring，io_uring 移入 P4）。
M1–M6 全部落地。Q1–Q7 决策已闭环：
- Q1 BufferHandle 归还 = RAII 析构自动归还
- Q2 AcquireBuffer 不清零（"内容未定义"，零拷贝路线前置契约）
- Q3 池耗尽立刻失败（invalid handle）
- Q4 `buffer_pool_count` 保持 slot 语义
- Q5 backend 选择 = 编译期宏 `CABE_IO_BACKEND_*`
- Q6 SPDK 路线保留为 P9
- Q7 Close + 未归还 handle = Debug abort / Release warn 强制释放

衍生约束:Q2=A 是零拷贝路线（P4 FIXED / P8 scatter-gather / P9 SPDK）的前置契约,
不能在后续阶段反复变更。

## P4 实施计划

设计稿:`doc/p4_io_uring_design.md`(2026-04-28 v1.0,讨论稿)。

P4 范围:io_uring 后端 + registered buffer pool。在 P3 抽象层基础上加
`IoUringIoBackend`,通过 `io_uring_register_buffers` + `*_FIXED` ops 兑现 P3 Q2
的零拷贝路线前置契约。公开 API 一字不变,异步化推到 P6。

P4 切成 9 个 milestone(M1–M9),按"先落地后优化"原则逐档独立交付,逐档独立
bench 归档:

- **M1** ✅ liburing 依赖接入 + IoUringIoBackend 骨架 + CMake
  `CABE_IO_BACKEND=io_uring` 取消 FATAL_ERROR + TSAN/io_uring 编译期阻断(D19)
- **M2** ✅ Open / Close + mmap pool + ring 真实实现;状态机 + Q7 行为对齐 sync 后端
- **M3** ✅ 朴素 WriteBlock / ReadBlock:Model A(粗 `io_mutex_`)+
  `submit_and_wait(1)`,无 FIXED,无 register_files;归档基线 `p4-pre-fixed`
- **M4** ✅ `io_uring_register_buffers` + `WRITE_FIXED` / `READ_FIXED`;
  BufferHandleImpl 加 `fixed_buf_index_` 字段;register_buffers 失败 → Open 失败(D15);
  归档基线 `p4-post-fixed`;cpu_time 加速 16-82%(small op GUP 消除受益最大);
  新增 `test/io/io_uring_specific_test.cpp::RegisterBuffersFailsWhenPoolTooLarge`
  (root 下 SKIP,需 setpriv 降权才能真实跑 D15 路径)
- **M5** ✅ `io_uring_register_files` + `IOSQE_FIXED_FILE`;Open 内 register fd 一次,
  WriteBlock/ReadBlock 走 `prep_*_fixed(fd_idx=0, ...)` + `sqe->flags |= IOSQE_FIXED_FILE`,
  Close 内 unregister_files 先于 unregister_buffers;失败 rollback 路径
  (D15 一致姿态):unregister_buffers → queue_exit → munmap → close fd;
  bench 归档名 `p4-post-fixed-files`(避免与 M4 的 `p4-post-fixed` 重名),
  预期 +1-3% cpu_time(fdget/fdput 摊销)
- **M6** ❌ `Options.io_uring_sq_depth`(D7 第二部分)+ Open 前置校验
  sq_depth >= pool_count(R12)+ io_uring 专属测试扩展(CloseDrainsInflight /
  RegisterBufferIndexMatchesSlot / OpenRejectsSqDepth / WriteBlockEAGAINRetries)+
  README "Production deployment notes"(ulimit / systemd `LimitMEMLOCK`,R7)+
  CABE_HAVE_* feature gate try_compile(D10)
- **M7** ❌ 内部 batch API(`WriteBlocks` / `ReadBlocks`)+ Engine 多 chunk 路径
  接入;归档基线 `p4-post-batch`(时机灵活,M5–M6 稳定后再做)
- **M8** ❌(可选)Model A → Model B 升级评估;触发条件:M7 数据显示 Model A
  是多线程 Get 吞吐瓶颈
- **M9** ❌ bench 归档总结 + README / 路线图收尾,设计稿状态 →「已实施」,
  P4 状态 → ✅ 完成

### 关键决策（D1–D19,完整论述见设计稿 §3）

- liburing 动态链接,`>= 2.9`(D11 / D12)—— 性能差异 ≈ 0,决策基于运维便利
- 1 ring / IoBackend 实例;Model A 起步,M7 后视情况评估 Model B(D2 / D14)
- P4 公开 API 保持 sync;异步化推到 P6 reactor(D3)
- buffer 注册粒度:n × 1 MiB iovec,`fixed_buf_index == slot_index`(D5 / D13)
- register_buffers 失败 → Open 失败,不 fallback 到非 FIXED 路径(D15)
- Close drain 不带超时(D17,与「健壮运行环境假设」一致)
- TSAN + io_uring 在 CMake 与 scripts 双层阻断(D19);ASAN / UBSAN + io_uring
  正常支持

### 引用的工程原则

- `feedback_engineering_principle.md` — **先落地后优化**:每档独立 milestone,
  逐档 bench 验证;Model A 在前,Model B 视数据再评估
- `feedback_robust_runtime_assumption.md` — **健壮运行环境假设**:drain 不带
  超时;不主动 fsync 校验持久化语义;不监测内核 worker punt;RLIMIT_MEMLOCK
  失败硬退出(不 fallback)

## 关键约束

- 仅支持 Linux（Fedora 43+），O_DIRECT + O_SYNC
- C++20（GCC 15+ 或 Clang 20+）
- 单机存储，无分布式需求
- TSAN 支持：`cmake -DCABE_ENABLE_TSAN=ON`

**原因：** 用户目标是推到多 NVMe 硬件瓶颈，最终迁移至 SPDK。
**应用方式：** 所有设计决策须保留无锁/异步演进路径，不能硬编码单设备假设。
