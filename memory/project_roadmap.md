---
name: Cabe 项目路线图与状态
description: Cabe KV 存储引擎各阶段完成情况、架构决策和关键约束
type: project
originSessionId: 5e75530e-aa98-44be-b0dc-01b60f844823
---

> **同步说明**:此文件是正本(README、设计文档引用它);Claude memory 镜像
> `~/.claude/projects/.../memory/project_roadmap.md` 供 Claude 会话自动读入。
> 最近一次同步:2026-05-15(P4.5 设计稿落地 + M1 实施后)。

## 完成状态（截至 2026-05-15）

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
- **P4** io_uring 后端 + registered buffer pool（io_uring 接管 BufferPool） — ✅ 完成（M1-M9 全部落地;M8 评估 = 不做）
  - 设计稿:`doc/p4_io_uring_design.md`(2026-04-28 v1.0 定稿 / 2026-05-14 已实施)
  - 进度:9 个 milestone 全部完成。M1-M7 代码 + 测试落地;M8 评估闭环 = 不做
    (Model B 在 P6 reactor 阶段被 per-thread ring 架构替代,不是 P6 中间形态);
    M9 收尾完成(bench 总结 `bench/baselines/p4-summary.md` + 文档 + 路线图)
  - bench 验证:M4 完成 cpu_time 加速 16-82%;M5 跑过 `p4-m5-post-fixed-files`
    baseline(cpu_time 落在 ±5% 测试环境噪声内,功能正确性由 contract test 保证);
    M7 大 value 批量 bench `p4-post-batch` 命令在 `bench/baselines/p4-summary.md`,
    用户 Linux 端跑完后补入 summary
  - 决策:**19 项全部闭环**(D7 第二部分 Options 字段 + D10 CABE_HAVE_* 在 M6 闭环;
    D14 Model A → M8 决策 = 不做;D18 批量 API 在 M7 闭环)
  - 风险:**12 项全部闭环**(R12 sq_depth >= pool_count 校验在 M6 与 Options 字段一并加)
  - 注释/文档同步状态:M1-M9 全部刷到 P4 收尾状态
  - 详见下文「## P4 实现摘要」与「## P4 实施计划」
- **P4.5** FreeList 改造(三容器轮换 + 严格升序分配 + 异步 sort + TRIM 集成)— 进行中
  (2026-05-15 设计稿落地 + M1 数据结构骨架已实施:`doc/p4.5_freelist_design.md`;
  5 个 milestone 中 M1 ✅ 完成,M2-M5 待实施;~7 天总工程量;详见下文「## P4.5 实施计划」)
- **P5** WAL + 崩溃恢复 — 计划中(2026-05-15 方向确定:**采用完整 WAL 方案**,
  排除纯 checkpoint / hybrid 路线,详见下文「## P5 方向决策」)
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
## P4 实现摘要（io_uring 后端 + registered buffer pool）

P4 范围:在 P3 抽象层基础上加 `IoUringIoBackend`,通过 `io_uring_register_buffers`
+ `*_FIXED` ops 兑现 P3 Q2 的零拷贝路线前置契约。公开 API 一字不变(D3),
  异步化推到 P6 reactor。

**M1–M9 全部落地**:
- M1-M7 代码 + 测试落地(详见下文「P4 实施计划」各 milestone)
- M8 评估闭环 = 不做(Model B 在 P6 路线图上无归宿,详见设计稿 §13 M8)
- M9 收尾完成(bench 总结 + 文档 + 路线图状态)

**D1–D19 决策全部闭环**(完整论述见 `doc/p4_io_uring_design.md` §3 + 附录 A):
- D1 / D11 / D12:liburing 动态链接 + `>= 2.9`(运维便利,性能差 ≈ 0)
- D2:1 ring / IoBackend 实例;多 NVMe 时多个 IoBackend 实例(P4+ 推进)
- D3:P4 公开 API 保持 sync,异步化推到 P6
- D4:M3 朴素 → M4 FIXED → M5 register_files 三档独立交付,逐档 bench 归档
- D5 / D13:n × 1 MiB iovec 静态注册,`fixed_buf_index == slot_index`
- D6:SQPOLL/IOPOLL 不进 P4(P6 reactor 阶段评估)
- D7:`Options.io_uring_sq_depth = 64`(M6 加,Open 校验 sq>=pool 且 2 幂)
- D8:错误码沿用 `IO_BACKEND_*` 七种,不新增
- D9 / D17:Close 必须 drain CQE,无超时(健壮运行环境假设)
- D10:`CABE_HAVE_IORING_SETUP_SINGLE_ISSUER` / `DEFER_TASKRUN` feature gate
  (M6,为 P6 reactor 单生产者独占 ring 路径预留)
- D14:Model A 起步;M8 评估 = 不做(P6 reactor 直接采用 per-thread ring 替代)
- D15:register_buffers 失败 → Open 整体失败,不 fallback
- D16:user_data 三档演进(M3-M6 = 0;M7 = 数组下标;P6 reactor = Request*)
- D18:M7 batch API + Engine 多 chunk 接入(P4 收尾阶段引入,时机灵活)
- D19:CMake + scripts 双层阻断 `io_uring + TSAN` 组合

**R1–R12 风险点全部闭环**:
- R1 RLIMIT_MEMLOCK 撞限 → D15 + README 部署文档 +
  `RegisterBuffersFailsWhenPoolTooLarge` 测试(root 下 SKIP)
- R2 TSAN false positive → D19 双层阻断
- R3 O_DIRECT|O_SYNC 持久化语义 → 健壮运行假设,不主动 fsync 校验
- R4 kernel worker punt → 不加 IOSQE_ASYNC,不监测
- R5 liburing 跨版本 breaking → pkg-config `>= 2.9` 锁定
- R6 M4 加速不及预期 → 实测 cpu_time 加速 16-82%,远超 5% 门
- R7 Hardened kernel / Docker seccomp 禁 io_uring → Open 返回 NOT_OPEN +
  README 列已知不支持环境(LXC 默认 / Hardened kernel `io_uring_disabled=2`)
- R8 多线程 SQ 竞争 → Model A `io_mutex_` 串行,M7 batch 同锁
- R9 Close drain 与 BufferHandle dtor 时序 → drain 完才解锁,Q7 force-release
- R10 queue_exit 与未消费 CQE → drain 收齐才 queue_exit
- R11 Engine/IoBackend 锁层级混乱 → §8.4 单向 + 注释文档化
- R12 sq_depth < pool_count 误解 → M6 Open 校验 sq_depth >= pool_count

**Q1–Q7 P3 衍生约束在 io_uring 后端的对齐**:
- Q1 BufferHandle RAII 归还 ✅
- Q2 AcquireBuffer 不清零 ✅(Q2=A 零拷贝前置契约)
- Q3 池耗尽 = invalid handle ✅
- Q4 buffer_pool_count = slot 语义 ✅
- Q5 backend 编译期 dispatch `CABE_IO_BACKEND_IO_URING=1` ✅
- Q6 SPDK 保留 P9 ✅(P4 不动)
- Q7 Close + outstanding = Debug abort / Release warn ✅(io_uring 后端镜像)

**关键性能数据**(M4 完成时实测,Fedora 43 + kernel 6.17.8 + Ryzen 9 9950X):
- cpu_time 加速 16-82%(small op GUP 消除受益最大)
- 远超设计稿 W4.6 验收门(≥ 5%)
- bench 归档:`p4-m4-pre-fixed` / `p4-m4-post-fixed` / `p4-m5-post-fixed-files`
- P4 各档 bench 对照见 `bench/baselines/p4-summary.md`

**留给后续阶段的接口**:
- **P5 WAL**:WAL 写就是普通 WriteBlock,BufferPool 已支持 metadata 类块
- **P6 reactor**:IoBackend 抽象层不变,reactor 内部直接采用 per-reactor ring
  (不沿用 Model B);`CABE_HAVE_IORING_SETUP_SINGLE_ISSUER` / `DEFER_TASKRUN`
  feature gate 预留接入点;M7 batch API + `user_data = 数组下标` codebase 可复用
- **P7 B+ 树**:节点 I/O 复用现有 BufferHandle 路径,自动享受 zero-copy
- **P8 scatter-gather**:M7 batch API 是 scatter-gather 的中间形态;真正零拷贝在 P8

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
- **M6** ✅ `Options.io_uring_sq_depth`(D7 第二部分,默认 64)+ Open 前置校验
  `sq_depth >= pool_count`(R12)与 `sq_depth` 是 2 的幂(D7);IoBackendTraits
  trait Open 加 3rd 参数 sqDepth(sync 后端忽略,签名一致);Engine 公开 API
  透传 Options.io_uring_sq_depth → 内部 Engine.Open → io_.Open;io_uring 专属
  测试新增 4 个 + 1 个 SKIP 占位;README "Production deployment notes" 章节就位
  (ulimit / systemd LimitMEMLOCK / Docker seccomp / Options.sq_depth);CMake
  CABE_HAVE_IORING_SETUP_SINGLE_ISSUER / IORING_SETUP_DEFER_TASKRUN feature gate
  (D10,为 M8 / P6 reactor 预留接入点)
- **M7** ✅ 内部 batch API(`WriteBlocks` / `ReadBlocks`)+ Engine 多 chunk 路径接入(D18):
  IoBackendTraits concept 加 span<pair<BlockId, [const] BufferHandle*>> 入参签名;
  io_uring 后端真实批量:`io_mutex_` 锁内 prep N 个 `prep_*_fixed` SQE(user_data = i)
  → `submit_and_wait(N)` → `io_uring_for_each_cqe` + 单次 `cq_advance(N)`,省 (N-1)
  次 syscall;sync 后端 for-loop 退化等价;Engine Put/Get/GetIntoVector 按
  `bufferPoolCount_` 分批,Phase A Acquire+memcpy+CRC → Phase B WriteBlocks/ReadBlocks
  → Phase C chunkIndex.Put / CRC校验+memcpy 输出;5 个 io_uring batch test 通过
  (Roundtrip / Empty / NullHandle / EquivalentToSerial / NotOpenError);
  bench 归档 `p4-post-batch` 留给 M9
- **M8** ✅(评估 = 不做)Model A → Model B 升级评估闭环。理由:
  (1) 架构定位错配 — P4 公开 API 仍为 sync(D3),Model B 内部异步化不改变用户视角
  的阻塞;(2) 设备容量已饱和 — 1 MiB 块 + 单 NVMe 下 Model A 单线程已逼近带宽极限;
  (3) **Model B 在 P6 路线图上无归宿** — P6 reactor 采用 1 ring / reactor 线程
  (§5.2 ring 拓扑演进表),单生产者单消费者无锁,直接绕过 Model B 的 reaper +
  inflight 表协议;(4) 复杂度成本约 700 行 + TSAN 不能验。详见
  `doc/p4_io_uring_design.md` §13 M8 决策结论
- **M9** ✅ bench 总结 `bench/baselines/p4-summary.md` + README Roadmap P4 → ✅ +
  本文件加 "P4 实现摘要" + 设计稿状态 → "v1.0 已实施" + §20 Open Questions 全部
  收口 + io_uring 后端顶部注释 P4 收尾标记

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

## P4.5 实施计划(2026-05-15)

设计稿:`doc/p4.5_freelist_design.md`(2026-05-15 v1.0 定稿,M1 已实施 / M2-M5 待实施)。
设计稿 § 14 包含每个 milestone 的完整接口表、数据结构、状态机、单测覆盖矩阵(M1.1-M5.6
共 30+ 子章节);此处为索引性质的摘要,展开见设计稿。

**P4.5 阶段定位**:P4 io_uring 后端收尾(2026-05-14)→ P5 WAL 之间的独立小阶段。
不并入 P5 的理由是数据结构改造的风险隔离 + 接口稳定先于持久化(P5 WAL recovery 依赖
`RebuildFromActive` 接口)+ 与"先落地后优化"原则一致。

**P4.5 范围**:
- 仅 `storage/free_list` 模块改造,不触动 Engine 业务逻辑、不引入持久化
- FreeList 从单 list 改为三容器轮换(freeList + 双 recycle),通过 idx swap 实现 O(1)
  角色变换(无 memcpy 合并)
- **严格升序分配**:freeList 用 `std::vector<BlockId>` 降序存储 + `pop_back()` 取最小
- **异步 sort**:后台线程在锁外执行 std::sort,业务路径只有 StartSwitch / CompleteSwitch
  的 O(1) idx swap 持锁(< 1 μs)
- **TRIM 集成**:Delete 路径同步 `ioctl(BLKDISCARD)`,失败 WARN 容错
- **`RebuildFromActive`**:P5 WAL recovery 用接口,P4.5 实现 + 单测

**核心决策**(完整 22 项见设计稿 §3 + 附录 A):
- D-1 / D-2:全量装载 [0, max) 到 vector,降序存储
- D-3:freeList 已用 90% 触发切换(`switch_ratio = 0.90`,从原 75% 调高,与 cabe 删除少
  的场景匹配)
- D-6:对称水位 `active.size() ≥ freeList.size() × 1.5` 任一满足即触发
- D-8:依赖 Engine 大锁,FreeList 不引入业务路径锁(只有 `worker_mu_` 处理后台协作)
- D-10:`FreeCount()` 保留"全局可用"语义,新增 `AllocatableCount` /
  `PendingRecycleCount` / `SwitchCount`
- D-NEW-1:`min_recycle_threshold = 1024`,化解启动后纯写入导致 freeList 变空的死锁
  风险(R-NEW-1)

**P4.5 milestone 划分**(总工程量 ~7 天,详细见设计稿 § 14):

- **M1** ✅ 数据结构骨架与接口(2026-05-15 已完成):
  - 接口:`SetMaxBlockCount` 触发全量装载(返回 int32_t,同 n 幂等)+ `Allocate`
    严格升序(降序 vector pop_back 取最小)+ `Release`/`ReleaseBatch` 进 active +
    7 个 Stats getter(FreeCount / Allocatable / PendingRecycle / Switch / MaxBlock /
    TrimSupported / IsSortInProgress)+ `SetTrimContext`/`SetTuning` 仅存储字段 +
    `RebuildFromActive` 返回 `CABE_INVALID_DATA_SIZE` 占位
  - 数据结构:`containers_[3]` + 3 个 idx + 5 个 atomic 状态字段(M2 起激活)+
    4 个 tuning 字段(默认 0.90/0.90/1.5/1024)+ 2 个 TRIM 上下文字段
  - 集成:`engine.cpp:70` 处理 SetMaxBlockCount 新 int32_t 返回值,失败回滚 `io_`
  - 单测:22 用例 / 两 fixture(`FreeListUninitTest` 6 个 + `FreeListTest` 16 个);
    覆盖 SetMax 契约、严格升序、Release 不立即复用、ReleaseBatch 原子性、Stats
    语义一致、未初始化路径、Stub 行为
  - 显式不做:切换触发(M2)/ 后台线程(M3)/ TRIM ioctl(M4)/ 写保护(M4)/
    完整 RebuildFromActive(M4)
  - 实际代码:~250 行新增 + ~280 行单测

- **M2** 同步切换路径(~1 天):
  - 新方法:`ShouldTriggerSwitch`(三条件复合 — `min_recycle_threshold` 前置 + freeList
    阈值 OR 对称水位 OR switch_pending);`StartSwitch`(`sort_task_` move + idx swap +
    state→Switching);`CompleteSwitch`(`sort_result_` move + idx swap + state→Running
    + switch_count++)。M2 阶段 sort 在调用栈内同步执行,M3 移到后台线程
  - 接口增量:`Allocate` 入口检测 sortDone → CompleteSwitch + pop 后检测触发 →
    StartSwitch;`Release` 检测对称水位设置 `switch_pending_`
  - 状态机激活:Running ↔ Switching 转换开始发生(M1 字段在位但不转换)
  - 私有成员:`sort_task_` / `sort_result_` 引入(M2 同线程使用,M3 跨线程)
  - 单测:7 类用例(freeList 阈值触发 / 对称水位触发 / min_recycle_threshold 阻止 /
    切换后仍升序 / 多次连续切换稳态 / 切换中状态查询 / 残余 2 轮等待生命周期)
  - 代码:~180 行新增 + ~250 行单测

- **M3** 异步 sort worker(~2 天):
  - 引入:`std::thread sort_worker_` + `std::mutex worker_mu_` +
    `std::condition_variable worker_cv_` + `std::atomic<bool> stop_`
  - 新方法:`SortWorkerFn` 后台主循环(cv.wait → 抢 sort_task_ → 锁外 std::sort →
    回填 sort_result_ → sort_done_=true);`SetMaxBlockCount` 装载后启动线程;
    `~FreeList` stop_+notify+join
  - StartSwitch 改造:从同步 sort 改为 lock worker_mu_ + move sort_task_ + notify_one
  - 跨线程协议:锁层级 Engine 锁 > worker_mu_,单向无死锁;atomic acquire/release
    配对 sort_done_;TSAN 干净
  - 单测:6 类(worker 生命周期 / 异步 sort 完成检测 / Switching 状态可观察 /
    持锁时间断言 < 几 μs / Close 切换中正确退出 / TSAN 全套 pass)
  - 代码:~220 行新增 + ~300 行单测

- **M4** 周边设施(~1.5 天):
  - TRIM 集成:`IssueTrim` 私有方法(`ioctl(BLKDISCARD, [id × CHUNK, CHUNK])`,
    失败 WARN 不影响业务返回);Release / ReleaseBatch 调 IssueTrim
  - 写保护:Allocate 入口判定 `available ≤ max × (1 - reject_ratio)` → NO_SPACE
  - `RebuildFromActive` 完整实现:等正在进行的切换完成 → 排序 active → 清空三容器 →
    用归并法构造 freeList = [0, max) - active(降序)→ 重置 idx / initial_capacity /
    switch_count
  - IoBackend 接口扩展:concept 加 `GetDeviceFd()`,sync / io_uring 各实现
  - Options 字段新增(4 个 freelist_* 字段)+ `Engine::Open` 入口校验(`switch_ratio`
    ∈ (0,1) 等)+ 透传 SetTuning / SetTrimContext
  - 单测:10 类(TRIM 检测/失败容错/不支持设备 + 写保护触发/边界 + Rebuild 基础/
    无序/全 active/切换中调用 + Options 字段校验)
  - 代码:~200 行新增 + ~280 行单测

- **M5** 文档与收尾(~1.5 天):
  - 旧测试调整:`test/engine/engine_test.cpp` / `engine_thread_test.cpp` 中含
    "BlockId 立即复用"隐式假设的用例改预期为"经切换后才复用"(`EngineCoverWrite` 等)
  - 新增 Engine 集成测试 5 个:`EngineFreeListIntegration_PutDeleteCycle` /
    `EngineSwitchUnderLoad` / `EngineTrimOnDelete` / `EngineWriteProtection` /
    `EngineRebuildFromActive`(为 P5 WAL 调用提供端到端验证)
  - 文档更新:设计稿状态 → `v1.0 已实施`;README Roadmap P4.5 → `✅ 完成`;
    本路线图 P4.5 完成状态 + 性能数据快照;新建 `bench/baselines/p4.5-summary.md`
  - bench 归档:`p4.5-baseline`,与 `p4-post-batch` 对比验证业务路径无回退(< 2%)
  - 回归测试:sync 后端 + io_uring 后端 + ASAN + TSAN 全套通过
  - 代码:~50 行 Engine 改动 + ~130 行旧测试调整 + ~250 行集成测试 + ~1500 行 markdown

**风险点全部 19 项闭环**(完整见设计稿 §4 + 附录 B):
- R-1 数据结构 → vector 降序 + pop_back
- R-13 FreeCount 语义 → 保留旧语义 + 新增 getter
- R-14 并发安全 → Engine 大锁 + `worker_mu_`
- R-NEW-1 启动后死锁 → `min_recycle_threshold` 化解(关键)
- R-NEW-3 后台线程生命周期 → 标准模式(stop_ flag + cv + join)

**性能特征**(40T 设备稳态):
- 切换间隔:30 分钟 - 数小时
- 单次切换持锁时间:< 2 μs(StartSwitch + CompleteSwitch idx swap)
- 系统锁占用率:~0.0000001%
- 业务路径几乎完全无阻塞

**留给 P5 的接口**:
- `RebuildFromActive(active_blocks)`:从 chunkIndex 中所有 active 的 blockId 集合
  反推 freeList(WAL recovery 后调用一次)。P4.5 实现 + 单测,P5 直接调用

**与 P5 WAL 的关系**:
- freeList 状态完全可从 chunkIndex 推导,**不需要 WAL 帧记录 freeList 切换 / Release**
- freeList 切换是纯内存行为,与 WAL 完全解耦
- 切换频率不影响 WAL 大小,WAL truncation 不影响切换语义

## P5 方向决策(2026-05-15)

**结论:采用完整 WAL 方案**,不做纯 checkpoint,不做 hybrid。

**决策依据**(用户在 2026-05-15 对话中确认路线图后段确定性):多 NVMe + reactor +
B+ 树 + 零拷贝 + SPDK + 可变长 value(通过参数配置全局 chunk size)都是必落地的演进项。
WAL 在这 6 个维度下全部为更优解;checkpoint 与零拷贝(序列化阶段对立)、stop-the-world
(reactor 延迟敏感)、多盘条带化一致性、小 chunk size 元数据爆炸等存在系统性冲突。

**演进路径意义**:
- WAL 路径 = 加层(P5 单线程 append → P6 per-reactor 段 → P7 redo log 嵌入 B+ 树
  → P8 walpayload 零拷贝 → P9 WAL-on-SPDK,类 BlueStore 模式)
- checkpoint 路径 = 推倒(P5 之后每一阶段都要部分推翻持久化层)

**排除项**:Bitcask 风格"数据文件即 WAL"在固定 chunk size 下不能直接套用
(Cabe chunk 是纯 value,无自描述帧头),但格式上可以借鉴 self-describing 帧的思路。

**P5 朴素版边界**(待设计稿正式确认):
- 单 WAL 文件,O_DIRECT 顺序 append
- 帧格式 `[type:1][len:4][crc:4][payload:N][seq:8]`
- 三种 entry:PutCommit / Delete / FreeListUpdate
- commit 协议:WAL 帧落盘 → chunk data 落盘 → index 更新 → 返回
- recovery:启动时全量 replay,重建 metaIndex / chunkIndex / freeList
- 单线程 append;不做 group commit、不做 truncation、不做 checkpoint 加速 recovery
  (这些留给 P6+ 增量加层)

**待起草**:`doc/p5_wal_design.md`(类比 P4 设计稿结构)。


## 关键约束

- 仅支持 Linux（Fedora 43+），O_DIRECT + O_SYNC
- C++20（GCC 15+ 或 Clang 20+）
- 单机存储，无分布式需求
- TSAN 支持：`cmake -DCABE_ENABLE_TSAN=ON`

**原因：** 用户目标是推到多 NVMe 硬件瓶颈，最终迁移至 SPDK。
**应用方式：** 所有设计决策须保留无锁/异步演进路径，不能硬编码单设备假设。
