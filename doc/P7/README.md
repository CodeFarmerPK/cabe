# P7 — Reactor 并发模型 + 无锁多线程 + 多 device 端到端 · 设计文档索引

> P7 阶段目标：把数据通路从调用线程**搬进 per-reactor 事件循环**——每 device 一个 reactor、
> 独占一份数据分区、share-nothing、消息传递、**无任何 mutex**；公开 API 签名冻结（内部异步对
> 用户透明）；多 device 端到端跑通。本阶段是一次"**重新安置**"：io / wal / snapshot / index /
> slots / 恢复 / 那段数据通路逻辑全部原样复用，新增的只有 reactor 外壳、同步包异步、路由真实化，
> 外加把 P6 的 group commit 留作死代码。阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P7"。

## 状态

⏳ **设计中**（阶段总体设计/计划已定——见下文决策表 P7-D1~D13；五个里程碑的详细设计待逐个 grill；代码未实装）

## 设计背景与定调

### P7 是"框架迁移"，不是"重写"

盘一遍工作区会发现：P7 真正新增的只有三样——**reactor 外壳、同步包异步的薄包装、两级路由**，
外加把 P6 的 group commit 简化（实为留作死代码）。其余全部原样复用：

- I/O 后端（sync / io_uring）**一行不改**——它们本就是单线程用法，P7 只是把"用它的线程"从
  调用线程换成 reactor 线程（io_uring 继续用现在的"提交即等待"，简单版正需要它阻塞）。
- wal / snapshot / index / allocator **逻辑不改**——变的是所有权（从 `DeviceContext` 共享变成
  reactor 独占）。
- 数据通路那段 `Acquire → io.Write → WriteWal → Insert → 回收 → 快照触发` **整段照搬**进 reactor，
  逻辑不动。
- recovery **不动**——它在 Open、线程启动前跑，仍是 per-device 串行。

这是 P7 风险可控的根本：新建的那点东西还都有 P6 的 `WriterNode` / `WaitResult` 模板可抄。

### 为什么首轮不纠结性能、"简单版"就够（红线复核）

两条性能红线在"简单版"（reactor 内一次跑完一个 op）下都能达成，所以**流水线不被红线逼出来**：

- **QPS 红线**（多线程峰值 ≥ 70%×N × P6 单线程）测的是"加设备能不能近线性扩展"。这里 N 只能是
  **设备数**（R=1 下单 reactor 串行，加调用线程不会让单设备 QPS 涨）。而基准 P6 单线程本身就是
  QD=1。简单版每设备一个 reactor 也是 QD=1 ≈ P6 单线程，N 个设备 share-nothing 并行 ≈ N × P6
  单线程，稳过 70%×N（30% 余量留给投递一跳和不完美扩展）。
- **p50 红线**（单线程劣化 ≤ 10%）：简单版单线程 = P6 op + 投递/唤醒一跳。相对 1MiB I/O（数百 µs）
  的主成本，一跳 futex 往返（µs 级）很小。

也就是说"打满单盘"才需要流水线，而红线根本没要求打满单盘、只要求 N 设备横向扩展。所以**流水线及其
那一串债（committed_seq 已提交水位、BufferPool 跨线程、FUA 改每笔 RWF_DSYNC、模糊/增量快照、同 key
乱序回收）整体推到性能兑现轮**。P7 首轮重在立起无锁 reactor 骨架 + 快速出原型，性能红线作**观察项**、
不作门槛（见 P7-D2）。

### 为什么没有 TSAN 证据空洞

P7 简单版**没有 io_uring 专属的异步并发面**——io_uring 后端还是"提交即等待"（submit + wait_cqe），
没上真异步的 cqe 收割与多在飞 op 状态机。所以 P7 的全部并发面都是**后端无关**的 reactor 投递 / 唤醒
（MPSC 入站队列 + futex），**sync + TSAN 完整覆盖**。之前担心的"TSAN 测不到 cqe 收割"那个空洞，属于
流水线，和流水线一起留到性能轮，P7 不存在。

## 范围摘要

- **per-reactor 状态分区**：每 device 一个 reactor（R=1），独占 `(IoBackend, BlockAllocator
  partition, MetaIndex partition, WAL queue endpoint)`；跨 device 零通信；**无任何 mutex**（D16/D18/D19）。
- **同步包异步**：调用线程算路由 → 栈上建 op → 投递入站 MPSC 队列 → 挂 thread_local futex →
  reactor 单线程处理 → 唤醒。公开 API（`Put`/`Get`/`Delete`/`SetWalLevel`/`Snapshot`/`Close`）
  签名一字不改（D17，API 自此真正冻结）。
- **数据通路**：现有 Put/Get/Delete 逻辑整段搬进 reactor 执行，逻辑不改。
- **路由真实化**：`RouteKey` 从恒 0 改为 `hash(key) % N`（用现成冻结的 `util::Hash`）；多设备启用
  （`BlockId` device 位 + `Open` 放开 `size>1`）。
- **运营口**：`Close` / `SetWalLevel` / `Snapshot` 降格为 fan-out op。
- **复用不改**：io / wal / snapshot / index / slots / recovery 代码原样；**group commit 留死代码**
  （reactor 单写者、批 1）。
- **不做（留性能兑现轮）**：流水线 + committed_seq + BufferPool 跨线程 + RWF_DSYNC + 模糊快照 +
  同 key 乱序回收 / R>1（device 内再分区）/ TRIM / 钉核·NUMA·调优 / io_uring 真异步·SQPOLL·
  DEFER_TASKRUN / group commit 拆壳清理。

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 工作量 | 状态 |
|---|---|---|---|---|
| M1 | reactor 机制（读路径）+ 同步包异步 | `P7M1_reactor_skeleton_design.md` | L | ⏳ 待设计 |
| M2 | 写路径入 reactor | `P7M2_write_path_design.md` | M | ⏳ 待设计 |
| M3 | 单设备多线程 | `P7M3_single_device_mt_design.md` | M | ⏳ 待设计 |
| M4 | 多设备 | `P7M4_multi_device_design.md` | M | ⏳ 待设计 |
| M5 | 故障隔离 + 收敛 | `P7M5_isolation_convergence_design.md` | S | ⏳ 待设计 |

> **里程碑命名为暂定**，最终标题随各里程碑 grill 时确定。M1 拆出"读路径"单独成一刀，是把 P7 最难调的
> 异步机制隔离进最薄的一刀，也是最早可运行、可观察的产物（详见 P7-D12）。

## 里程碑依赖

```
P7M1 ──► P7M2 ──► P7M3 ──► P7M4 ──► P7M5      （严格串行）
读机制    写路径    单设备多线程  多设备     隔离+收敛
```

每一刀只新引入一类风险、都能独立编译运行、都是一个观察点：M1 = 异步机制（接缝）、M2 = 写路径重新
安置、M3 = 并发、M4 = 多设备并行、M5 = 隔离。

## 启动条件

1. ✅ P6 全部完成（group commit 机制已实装并收敛，P6 与 P4/P5 同级收尾）
2. ✅ P7 阶段总体设计完成（本文档，决策 P7-D1~D13）
3. ⏳ 各里程碑详细设计待逐个 grill（M1 起）

## 已锁定决策（P7 决策梳理）

| 编号 | 决策 | 结论 |
|---|---|---|
| P7-D1 | 定位与并发模型 | **框架迁移**：数据通路从调用线程搬进 per-reactor 事件循环，组件原样复用。per-reactor 状态分区、跨 device 零通信、**无任何 mutex**（D16/D18/D19）；公开 API 签名冻结、内部异步对用户透明（D17，冻结自然成立——签名不变）。 |
| P7-D2 | 性能取向 | **首轮不纠结性能**：两条红线（单线程 p50≤10%、多线程 QPS≥70%×N）作**观察项**、非退出门槛。简单版（reactor 内一次跑完一个 op）即可过红线（QPS 红线测 N 设备横向扩展、基准本就 QD=1；p50 一跳投递相对 1MiB I/O 很小）。流水线及其债务整体推到性能兑现轮。 |
| P7-D3 | Reactor 形态与归属 | Reactor 放 `engine/`（普通类，**非模板/concept**）；组合 `{一个 DeviceContext, 入站队列, 工作线程}`，暴露 `Start`/`Stop`/`Submit` + 私有 `Run`。放 engine/ 而非独立模块：单独成库会与 `device_context.h`（及其依赖 backend_config/buffer_pool/super_block）撞循环依赖；Reactor 是 engine 内部执行机制、非可插拔数据组件。 |
| P7-D4 | DeviceContext 所有权 | Reactor **独占持有** DeviceContext。Open 在裸 dc 上 recover 完 → move 进 reactor → Start。Engine 改持 `vector<unique_ptr<Reactor>> reactors_`（Reactor 不可移动，内含运行线程引用 this）；`devices_` 消失。交接后 dc 是 reactor 私有、只能走 Submit 访问——share-nothing 成**类型层面保证**。否决"Engine 持 devices_、reactor 借引用"（借引用拦不住调用线程越界，io_uring 不兼容 TSAN 测不出）。recover 仍在 Open、线程起之前单线程做。 |
| P7-D5 | op 描述符 + 入站队列 | OpNode 建调用线程栈上、零堆分配（复刻 P6 `WriterNode`）：`{OpType, 输入视图(key/value/buffer/level), atomic<int32_t> result(kPending 哨兵), next, wake_ptr}`，输入用视图不拷贝。入站队列复用 P6 MPSC Treiber 栈（`PushWriter`/`DrainAll`/`ReverseChain`），**但无 leader 选举**（reactor 是唯一固定消费者）。Run 循环 = 等非空 → 整批 exchange → 反转到达序 → 逐个处理 → 逐个唤醒。**数据 op + 运营 op 全走同一队列**（§5.3 竞态被无锁还清的机制）。 |
| P7-D6 | 同步包异步唤醒 | futex（C++20 `atomic::wait/notify`，复用 P6）。**唤醒挂点 = 每调用线程一个 thread_local 长寿字、与 per-op 结果槽分离**（避开 P6"最后触碰"UB：reactor notify 前调用栈可能已析构）；OpNode 只带指向该字的指针。reactor 收尾：存 wake_ptr 本地 → `result.store(release)` → wake_ptr `fetch_add(release)` + `notify_one`。`notify_one` 精确唤醒（顺带消 P6 惊群）。reactor 空闲时 futex-wait 在 inbox head、pusher notify。**不做 spin-then-wait**（性能优化、首轮不做）。 |
| P7-D7 | 两级路由 | 路由 hash 用现成冻结的 `util::Hash`（XXH3，D6）+ `RouteToDevice`。两级公式 `device=hash%N`、`reactor_within=(hash/N)%R` 作**模型**写文档；**R=1 全程，代码只实现设备级 `hash%N`**（第二级是 R=1 死代码、不写）。`RouteKey` 返回**扁平 reactor 下标**（前向兼容形态），N==1 短路返回 0。路由到设备 i 与 `BlockId.dev()==i` 对齐（M4 三处一致化）。 |
| P7-D8 | 运营口语义 | Close/SetWalLevel/Snapshot **全降格为投递 op**、走同一队列、**fan-out 到 N 个 reactor**、首个错误聚合（不改签名/Status）。**SetWalLevel 的 wal_level 改 per-reactor 状态**（每 reactor 自己的 Options 副本）消跨线程读写 race；其余 Options 字段 Open 后只读、共享读安全。自动快照（`MaybeRequestSnapshot`）不需 fan-out（本就在 reactor 自己线程、是 op 尾段）。 |
| P7-D9 | 背压 | **不做显式背压**。同步 API + 栈驻节点 → 在飞 op ≤ 调用线程数、每 reactor 入站队列最多 M 个栈节点、涨不爆。队列无上限、不阻塞投递、不加 busy 错误码、不加防御 sanity 上限（依赖同步 API 的天然有界，D17 冻结保证其稳定）。 |
| P7-D10 | group commit | **P7 全程不碰**。reactor 单写者调现有 `WriteWal`（自任 leader、批 1、零改动、正确）；其并发机械（leader 选举 / MPSC 栈 / 4 个 seq_cst / epoch）全程是死代码、留着无害。拆壳清理 + 真批量 fdatasync + RWF_DSYNC 一并留性能轮的 WAL 写路径重写。设计稿记一笔说明。 |
| P7-D11 | reactor 生命周期 | **不钉核**（性能轮）。Open：**全设备 recover 成功后再起 reactor**（失败路径最简）。Close = **drain-then-close**（`opened_=false` → fan-out Stop op → 处理完 Stop 前的 op、跑关闭序列(关 snapshot→wal→io)、唤醒 Stop 后掉队 op 成失败、退出 → join + 聚合）。不变量：**reactor 停止必唤醒所有等待者**（防孤儿）；异常死亡是防御角落（no-fault-injection）。per-reactor 独立生命周期支撑 M5 隔离。 |
| P7-D12 | 里程碑划分 | **5 个严格串行**（M1 读路径机制 → M2 写路径 → M3 单设备多线程 → M4 多设备 → M5 隔离+收敛）。M1 拆出读路径单证异步机制：它是 P7 最难调的层、也是最早可运行可观察的产物；写路径 M2 再搬。 |
| P7-D13 | P7 之外推迟 | 流水线及其债务（committed_seq / BufferPool 跨线程 / RWF_DSYNC / 模糊快照 / 同 key 乱序）、R>1（device 内再分区）、TRIM（用户确认 P7 不做）、钉核·NUMA·false-sharing·spin-then-wait、io_uring 真异步·SQPOLL·DEFER_TASKRUN、group commit 拆壳——全留**性能兑现轮**。 |

## 各里程碑范围

### P7M1（reactor 机制 · 读路径）—— N=1，单 reactor，单调用线程

**范围**：
- 新建 `engine/reactor.{h,cpp}`（P7-D3）：Reactor 类、入站 MPSC 队列（P7-D5）、工作线程、`Run` 事件循环。
- op 描述符 OpNode + `Submit`（P7-D5），数据侧**只支持 Get**（最简路径：单 I/O、无 WAL、无回收）+ Stop。
- 同步包异步唤醒（P7-D6）：thread_local wake_gen + 结果槽分离、`notify_one`、reactor 收尾序列、
  reactor 空闲 futex-wait。
- 所有权与启停（P7-D4/D11）：Engine 持 `vector<unique_ptr<Reactor>>`（N=1）、recover→move→Start、
  Close = Stop + join。`RouteKey` 此时返回 0（N=1，P7-D7 短路）。recover 仍在 Open 单线程跑。

**退出条件**：① 单调用线程经 reactor 完成 Get，结果等价原同步 Get；② recover 后经 reactor Get
取到恢复数据（新结构下恢复仍干净）；③ Close drain-then-close + join 干净退出，无等待者孤儿、无 fd
泄漏；④ **caller↔reactor 跨线程交接 race-free**（sync + TSAN）；⑤（观察）单线程 Get 的 p50，只记录、
不卡门槛。

**待梳理决策点**（留 M1 grill）：OpNode 精确字段布局；reactor 收尾序列与 wait 循环的精确写法；Reactor
类接口；Open 失败 / 起线程失败的精确清理。

### P7M2（写路径入 reactor）—— N=1，单 reactor，单调用线程

**范围**：
- Put/Delete 写路径整段搬进 reactor（`Acquire → pool → memcpy/CRC/时间戳 → io.Write → WriteWal →
  Insert → 回收 → MaybeRequestSnapshot`，逻辑照搬）。
- `WriteWal` 用现有的（P7-D10，自任 leader 批 1）；`WriteWalRescuing` 在 reactor 上跑（单写者下
  "重试恰一次必成"成立）；`MaybeRequestSnapshot` 在 op 尾段、reactor 线程上（P7-D8，不需 fan-out）。
- `BufferPool` 成为 reactor 私有（reactor 线程 Allocate/用/Free，单线程无锁）。
- 运营口 `SetWalLevel` / `Snapshot` 的单 reactor 形态（fan-out 到 N=1 即投递给唯一的 reactor）。

**退出条件**：① 单调用线程经 reactor 完成 Put/Get/Delete 全套，等价原同步引擎；② 覆盖写、
Delete-then-Put、capacity exhaustion（`kEngineNoSpace`）、CRC 比对等现有 `engine_test` 场景全过；
③ recover 后经 reactor 跑写路径 + 再 recover，数据一致；④ 写路径交接 race-free（sync+TSAN）；
⑤（观察）单线程 Put/Get/Delete p50。

**待梳理决策点**：写路径搬迁时 buffer / 时间戳 / 旧块回收时序的精确照搬；`SetWalLevel` 的 per-reactor
Options 副本布局与"收紧档先刷自己的 wal"的落点。

### P7M3（单设备多线程）—— N=1，单 reactor，多调用线程

**范围**：
- 入站队列升级到真多生产者并发（P7-D5，多 caller CAS push；P6 `PushWriter` 同款）。
- 并发不丢唤醒（P7-D6，`WaitResult` 四步、per-caller thread_local wake_gen、`notify_one`）。
- **§5.3 三竞态偿还**（P7-D8）：运营 op（SetWalLevel/Snapshot/Close）与并发 Put 的 WriteWal 在 reactor
  串行、不加锁。
- `kWalFull` 救援"重试恰一次必成"在多 caller 下仍成立（reactor 串行）；背压依赖同步 API 天然有界
  （P7-D9，不做机制）；group commit 不碰（P7-D10）。

**退出条件**：① 多调用线程并发砸单 reactor，所有 op 正确、**无丢唤醒、无 race**（sync + TSAN 多轮）；
② §5.3 三竞态在 reactor 串行下消失的实证（SetWalLevel/Snapshot/Close 与并发 Put 交错，结果一致不崩）；
③ 多线程下数据一致（并发 Put/Get/Delete + recover，盘面/索引一致）；④（观察）多 caller 单 reactor
吞吐——R=1 单 reactor 就是串行、吞吐≈单线程，不指望扩展（扩展是 M4 加设备）。

**待梳理决策点**：并发测试的形态（直打 reactor 还是经 Engine 公开 API）；§5.3 三竞态的具体测试场景。

### P7M4（多设备）—— N≥2，每 device 一个 reactor，R=1

**范围**：
- **三处一致化**：`RouteKey` 真实化 `hash%N`、`BlockId` device 位 `Init(static_cast<DeviceId>(i))`、
  清 `ValidateRecoveredMeta` 的"device 位非零即拒"、`Open` 放开 `size>1`（P7-D7）。
- N 个 reactor 并行、跨 device 零通信。
- 运营口 **fan-out 真正生效**（P7-D8）：Snapshot/Close/SetWalLevel scatter 到 N 个 reactor、gather、
  首个错误聚合。
- 多设备恢复：N 个 device 各自独立 recover（Open 里串行驱动，各落各分区）。
- `meta.block.dev()==RouteKey` 一致性断言。

**退出条件**：① N=2 端到端 Put/Get/Delete 正确，key 按 hash 分散、同 key 恒落同一设备；② 两设备各自
recover 后数据一致（含跨重启 Put/Delete/Snapshot）；③ 运营口 fan-out 正确（触达全部 N 个、首个错误
聚合）；④ 三处一致化无错位（路由到设备 i 取到的块 `dev()==i`）；⑤（观察）QPS 随设备数扩展——只记录，
loop 设备上这数不可信，真盘留 P11。

**待梳理决策点**：三处一致化的具体改动顺序与断言落点；运营口 fan-out 的 scatter-gather 实现；多设备
恢复是串行驱动还是并行。

### P7M5（故障隔离 + 收敛）—— N≥2，R=1

**范围**：
- 补全"reactor 停止必唤醒所有等待者"不变量（P7-D11 ④；正常停机已在 M1 Close 有，这里确认 + 演示）。
- by-construction 隔离论证（share-nothing、跨 device 零通信）+ **可控明显故障的轻量演示**（如某设备
  Open 失败、其 reactor 不起、其它继续服务），**不注入运行时 I/O 故障**（no-fault-injection）。
- P7 收敛稿（收口 P7-D1~D13 决策回链、五里程碑、退出条件、与 P11 分界）。

**退出条件**：① 一个设备出明显故障（如 Open 失败），其它设备的 reactor 继续正常服务（隔离演示）；
② 等待者不孤儿（reactor 停止必唤醒所有挂着的调用线程，Close 路径已证）；③ P7 全量回归（sync 四档 +
io_uring；TSAN 在 sync）全绿；④ 收敛稿审阅通过 + 状态同步。

**待梳理决策点**：隔离演示的具体明显故障手法；收敛稿形态（薄索引，参照 P5M7/P6M3）。

## P7 之外（留性能兑现轮）

- **流水线（多 op 在飞）** + 它那串：`committed_seq` 已提交水位、BufferPool 跨线程、FUA 改每笔
  `RWF_DSYNC`、模糊/增量快照、同 key 乱序回收。
- **R>1**（device 内再分区 MetaIndex/BlockAllocator）+ 第二级路由代码。
- **group commit 拆壳清理 + 真批量 fdatasync** —— 并入性能轮的 WAL 写路径重写（P7-D10）。
- **TRIM** 三处空桩（用户确认 P7 不做）。
- **钉核 / NUMA / false-sharing / spin-then-wait / io_uring 真异步用法 / SQPOLL·DEFER_TASKRUN** ——
  全是性能轮。

## P7 退出条件概要

1. 数据通路经 reactor 完成（读 M1、写 M2），单调用线程下结果等价原同步引擎、recover 仍干净
2. 多调用线程并发无锁安全：无丢唤醒、无 race（sync + TSAN），§5.3 三竞态在 reactor 串行下消失（M3）
3. 多 device 端到端正确：两级路由、N reactor 并行、运营口 fan-out、多设备各自恢复（M4）
4. 故障隔离：一个设备明显故障不连累其它；等待者不孤儿（M5）
5. P7 全量回归全绿（sync 四档 + io_uring；TSAN 在 sync）+ 收敛稿落定（M5）

> 两条性能红线（单线程 p50≤10%、多线程 QPS≥70%×N）作**观察项**，每里程碑测了记录、**不作退出门槛**
> （P7-D2）；真盘上的规模/带宽度量留 P11。

## 验证策略

- **并发正确性靠 sync + TSAN**：P7 的并发面全是后端无关的 reactor 投递/唤醒（MPSC + futex），sync
  后端 + TSAN 完整覆盖。io_uring 不兼容 TSAN（P4M3 既定），但 P7 简单版没有 io_uring 专属的异步并发面
  （还是"提交即等待"），所以不存在 TSAN 证据空洞（那个空洞属于流水线、随流水线留性能轮）。
- **io_uring 是性能锚点**但 P7 不纠结性能：功能正确性两后端都测，性能数据只观察、不调优。
- 检测器矩阵沿用 cabe 既定：sync 四档（asan/tsan/ubsan/release）+ io_uring（asan/ubsan/release），
  io_uring + TSAN 排除。

## 与 P11 的分界

P7 = **把多设备做出来**（架构/能力，小 N、loop 设备、正确性优先）；P11 = **在真盘上大规模验证 + 运维**
（N≥8、聚合带宽线性度、key 分布偏斜、深度故障隔离、运维文档），**不新增架构**。多设备的规模/性能度量
天然属 P11（真盘上才有意义），与 P7 不纠结性能的取向一致。

## 关键技术备忘

1. **简单版 = 每 op 一次 fdatasync，不是退化**：reactor 一次跑完一个 op（写值→WAL→fdatasync→Insert
   →下一个），所以同步档每 op 一次 fdatasync。这和 P6 单线程（批 1）一致、不比基准差；真正的批量
   fdatasync 要靠流水线攒多 op，属性能轮。
2. **reactor 是它那份 WAL 的唯一写者**：M3 加的是多个调用线程，但它们往 reactor 投 op；真正调
   `WriteWal` 的始终只有 reactor 线程一个。所以 group commit 的并发机械从 M1 起全程是死代码（P7-D10）。
3. **唤醒挂点必须比 op 节点活得久**：reactor 在 `result.store` 之后 notify，而调用线程可能已读到结果、
   返回、栈帧（含 OpNode）析构——所以挂点放 thread_local 长寿字、不放 per-op 栈节点（P7-D6，迁移 P6
   "最后触碰条款"）。
4. **share-nothing 是类型保证、不是口头约定**：reactor 独占持有 DeviceContext（P7-D4），Engine 物理
   上拿不到 dc、调用线程更碰不到，一切访问走 Submit——这是 io_uring 不能跑 TSAN 时唯一可靠的越界防线。
5. **三处一致化必须同批**（M4）：`RouteKey` 路由到设备 i、`BlockId` device 位、`Open` 放开三者只要不
   对齐，Get 就会按错设备读盘且 CRC 未必兜得住（可能读到另一份合法 value）。

## 命名与目录约定

沿用 `P<阶段>M<里程碑>_<主题>_design.md`。参见 [doc/P0/README.md](../P0/README.md)。
