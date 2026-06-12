# P6 — Group Commit · 设计文档索引

> P6 阶段目标：将 WAL 同步档（级别 1/3）的"每帧一次 fdatasync"改造为"积攒帧 + 共享 flush"
> 的提交组（group commit）机制——多个并发写者的 WAL 落盘合并为一次盘写 + 一次 fdatasync。
> 公开 API 维持单线程约束不变；本阶段交付的是**机制**，并发收益的主体在 P7 多 reactor
> 接入后兑现。阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P6 — Group Commit"。

## 状态

🚧 **设计中**（阶段计划已定，里程碑设计稿随 grill 逐个自建）

## 设计背景与决策依据

### 核心矛盾：单线程约束下的 group commit 怎么立足

ROADMAP 同时约定了"P2–P6 单线程访问，多线程语义在 P7"与 P6 的"提升并发写吞吐"。
两者的调和（P6-D1，**机制先行**）：

1. **单线程下没有可合并的 fsync**——同步 API 一次只有一个 Put 在等落盘，第二个
   fsync 根本不存在。Group commit 的收益主体（多写者攒批）要到 P7 多 reactor 并发
   提交时才出现。
2. **但机制必须先于使用者就位**——P7 的 reactor 改造工作量已经很重（无锁分区、
   消息传递、API 异步化），如果 WAL 提交协议也挤在 P7 做，两件高风险改造互相纠缠。
   P6 先把提交组协议在单线程环境下建好、用多线程 harness 直打 `Wal` 模块验证并发
   正确性，P7 的 reactor 即插即用——这正是 ROADMAP 把 P6 排在 P7 之前的用意。
3. **公开 API 不破墙**——Engine 的单线程约束原样维持；P6 的"并发"只存在于
   `Wal::WriteWal` 的模块级契约和测试 harness 里。

### 为什么选 leader/follower 而不是 batching window

ROADMAP 给了两个候选（倾向 leader/follower），P6-D2 定案 leader/follower：

1. **单线程退化路径 = 现行为**：无人竞争时，调用者自任 leader、直接写盘返回——
   与 P5 的 `Append + SyncCurrentBlock` 路径仅差几个原子操作。性能红线
   （p99 劣化 ≤ 20%）由协议结构保证，而不是靠调参。
2. **batching window 的人为延迟不可接受**：窗口等待对单线程是纯损失（每个 Put 都
   白等一个窗口期），p99 红线结构性必破；窗口长度还引入一个没有正确答案的调参维度。
3. **无后台线程**：leader/follower 由写者自己轮值充当 flusher，不需要常驻后台线程——
   与 P5 "无后台线程，异步化全归 P7" 的现状一致，TSAN 矩阵也不需要新增豁免。
4. **天然的负载自适应**：竞争越激烈 batch 越大（合并收益越高），无竞争时 batch=1
   （延迟最低）——不需要任何配置项。

### 为什么 tail latency 不做显式配置

ROADMAP 范围条目 4 写的是"batch size + 最长等待时间上限"。P6-D5 对账后裁决：
**两者都由协议结构天然满足，不引入配置项**——

- batch size 自然封顶于一窗容量（WAL 缓冲一窗的帧数，现状 4K 块 32 帧/块 ×
  缓冲块数）；超出的写者留给下一任 leader。
- follower 的最长等待 = 前任 leader 的一次盘写 + fdatasync，天然有界；没有
  batching window，就没有"最长等待时间上限"要配。

这是对 ROADMAP 原文的修正性裁决（原文按 batching window 的形态预写了配置项），
随本阶段状态同步回写 ROADMAP。

## 范围摘要

- **提交组协议**：leader/follower——写者把帧挂入提交队列；队列由空变非空者当选
  leader，独占盘写权，将在位期间到达的全部帧合并为一批：批量 `Append` + 整块写 +
  **一次 fdatasync**；follower 挂起等待，leader 完工后按帧回填各自结果并唤醒
- **lock-free MPSC 提交队列**：侵入式链表栈（写者 CAS push，leader 一次 exchange
  取走整批），节点活在调用者栈上、零堆分配；等待原语 = C++20
  `std::atomic::wait/notify`（Linux futex）
- **作用范围**：仅 WAL 同步档（级别 1/3）的 `WriteWal` 路径；攒批档（级别 2/4）
  本来就不每帧 fsync、无可合并对象，路径不动
- **接口零改动**：`Wal::WriteWal(const WalEntry&)` 签名与阻塞语义不变，Engine 零改动；
  seq 分配点从写者上下文移入 leader（单点分配，保住"活区间帧紧凑连续"不变量）
- **盘上格式零改动**：128B 帧、4K 整块写、环形推进、变体 Y 留窗、恢复逻辑全部不动——
  leader 批量落盘后的盘面形态与 P5 单帧逐写的盘面**逐字节等价**
- **失败语义**：盘写 / fdatasync 失败，整 batch 全体成员同收 `kWalWriteFailed`；
  `kWalFull` 准入仍逐帧——批内撞墙时已准入的帧照常落盘，被拒帧单独收 `kWalFull`
- **性能红线**：p99 单 Put 延迟相对 P5 劣化 ≤ 20%（单线程口径，见下文测量口径）
- **不做**：batching window / 定时器（D5 裁决不采）/ 级别 2/4 并发化（P7）/
  `WriteWal` 与 Flush/Reclaim/Close 的跨操作并发（P7）/ WAL I/O 异步化（P7）/
  外部多线程 API（P7）/ 性能基准归档（发版后补）

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | 提交组协议 + MPSC 队列 + Wal 同步档改造 | `P6M1_commit_group_design.md` | ⏳ 待设计 |
| M2 | 并发收口——失败语义 + 全局互动对账 + 测试矩阵 | `P6M2_concurrency_audit_design.md` | ⏳ 待设计 |
| M3 | P6 收敛——红线验证 + 状态同步 | `P6M3_convergence_design.md` | ⏳ 待设计 |

## 里程碑依赖

```
P6M1 ──► P6M2 ──► P6M3
```

严格串行：机制核心 → 并发收口 → 收敛。

## 启动条件

1. ✅ P5 全部完成（持久化与崩溃恢复已实装，P5M7 收敛通过）
2. ✅ P6 决策梳理完成（D1~D9，见下表）
3. ⏳ 用 `/grill-with-docs P6M1` 开第一个里程碑的详细设计

## 已锁定决策（P6 决策梳理）

| 编号 | 决策 | 结论 |
|---|---|---|
| P6-D1 | 定位与并发边界 | **机制先行**：公开 API 维持单线程；提交组建在 `Wal` 模块内部，成为级别 1/3 的唯一提交路径；并发正确性由多线程 harness 直打 `Wal` 验证；P7 reactor 即插即用 |
| P6-D2 | 提交协议 | **leader/follower**（采 ROADMAP 推荐）：单线程退化 = 现行为、无后台线程、无人为窗口延迟、负载自适应 |
| P6-D3 | 提交队列与等待原语 | 侵入式链表 MPSC 栈（CAS push + leader exchange 整批取走，入栈逆序由 leader 反转还原到达序）；节点在调用者栈上零分配；等待 = C++20 `std::atomic::wait/notify`（futex） |
| P6-D4 | 盘上格式与几何衔接 | 盘上格式 / 环形推进 / 变体 Y / 恢复逻辑**零改动**；leader 复用 `Append` + 整块写，批跨多块则多块一次写出 + 单次 fdatasync；盘面形态与 P5 逐帧路径逐字节等价 |
| P6-D5 | tail latency | 不引入 batching window 与定时器；batch 封顶 = 一窗容量，follower 最长等待 = 前任 leader 一次盘写，皆由协议结构天然有界（修正 ROADMAP 预写的配置项形态） |
| P6-D6 | 失败语义 | 盘写/fsync 失败整批同收 `kWalWriteFailed`（每个 waiter 独立回填结果）；与 P5 单帧失败语义同构、不新增承诺；`kWalFull` 准入逐帧，批内撞墙已准入者照常落盘 |
| P6-D7 | 四级持久化关系 | 仅级别 1/3 走提交组；级别 2/4 攒批路径不动（无每帧 fsync 可合并），其并发化随 P7；`SetWalLevel` 切档与提交组的互斥归 M1 设计 |
| P6-D8 | 红线测量口径 | P6 启动时先采 P5 终态单线程基线（engine_bench `BM_Put`、级别 3），完工后同机同参对比 p99；一次性对比不归档（承 P5M7-D5 口径）；另做多线程 `Wal` 吞吐 bench（信息性、无红线） |
| P6-D9 | 里程碑划分 | 3 个里程碑（机制核心 → 并发收口 → 收敛）；并发契约边界 = "多写者 `WriteWal` 并发安全；`WriteWal` 与 Flush/Reclaim/Close 的并发归 P7" |

## 各里程碑范围

### P6M1（提交组协议 + MPSC 队列 + Wal 同步档改造）

**范围**：
- 提交队列数据结构：侵入式 MPSC 栈 + 写者节点（帧载荷 + 结果槽 + 等待原子）
- leader/follower 协议实装：当选规则（队列空→非空者为 leader）、在位期间的领导权
  连任循环（drain → 写盘 → 复查队列）、卸任与交接
- `WriteWal` 同步档分支改走提交组；seq 分配移入 leader drain 时刻
- 批量落盘：批内帧按到达序 `Append`，跨块时多块一次写出 + 单次 fdatasync；
  与窗口推进 / 贴缝截短 / 恒留一块准入的衔接
- 单线程全量回归（现有测试零退步）+ 多线程 harness 直打 `Wal`（N 写者并发，
  断言全成功、seq 稠密、恢复扫描重放计数对账）

**待梳理决策点**：
1. 领导权交接竞态的封死方案——前任 leader 盘写期间新写者到达：busy 标志自旋？
   push 后 CAS 自荐？卸任后复查一次队列？（经典竞态：leader drain 后队列短暂为空，
   新写者误判自己当选 → 双写者；M1 设计稿必须给出证明级的状态机）
2. 批内 `kWalFull` 撞墙的精确切点——撞墙帧之后同批的帧是直接拒（保到达序语义）
   还是继续准入（环可能还有零头）？
3. Engine 撞墙救援（强制快照）在 follower 收到 `kWalFull` 后的重试路径是否原样成立

### P6M2（并发收口——失败语义 + 全局互动对账 + 测试矩阵）

**范围**：
- 失败语义批传播实装与验证：整批 `kWalWriteFailed` 回填、失败后 Wal 状态与
  M5/M6 不变量的对账（缓冲滞留、窗口状态、seq 账目）
- 与全局机制的互动对账（单线程时序下逐项过）：快照前 `Flush()`、`reclaim_boundary`
  捕获、`ReclaimUpTo` 回收、`SetWalLevel` 切档、`Close`——确认与提交组的执行序
  约束，并发互斥明确记账推 P7
- 恢复对账：并发写出的盘面经 M6 恢复链完整重放（端到端测试）
- TSAN 矩阵：多线程 harness 在 TSAN 下全绿（sync 后端）；四档 sanitizer 回归

**待梳理决策点**：
1. 失败后提交组是否进入"全拒"态（后续写一律 `kWalWriteFailed`）——对齐 P5
   "失败不动状态、滞留重试"语义还是收紧
2. 多线程 harness 的形态——独立测试目标还是并入 wal_test

### P6M3（P6 收敛——红线验证 + 状态同步）

**范围**：
- 红线验证：P5 基线 vs P6 终态，单线程 `BM_Put`（级别 3）p99 对比，劣化 ≤ 20%
- 多线程 `Wal` 吞吐数据采集（信息性：1/2/4/8 写者的合并率与吞吐曲线）
- 薄收敛稿：决策回链、不变量汇总、债务流向、退出条件核销
- 状态同步：本 README / ROADMAP P6 段 / 根 README；按惯例不引入 P7 占位

## P6 退出条件概要

1. 提交组协议实装，级别 1/3 经由提交组提交；单线程现有测试零退步
2. 多线程 harness 直打 `Wal` 全绿（含 TSAN），seq 稠密性与恢复对账成立
3. 失败语义兑现：整批失败同收 `kWalWriteFailed`，失败后状态与不变量对账通过
4. 并发写出的盘面经 M6 恢复链完整恢复（端到端）
5. 性能红线达标：单线程 p99 相对 P5 基线劣化 ≤ 20%
6. P6M3 收敛稿审阅通过 + ROADMAP / README 状态同步（含 D5 对 ROADMAP 原文的修正回写）

## 关键技术备忘（来自决策梳理）

1. **单线程退化路径是红线的结构保证**：无竞争时调用者自任 leader 直写直返，
   相对 P5 仅多几个无竞争原子操作（约纳秒级），p99 红线不靠调参。
2. **seq 单点分配**：P5 的 seq 在 `Append`（写者上下文）分配，单线程下安全；
   并发后必须收口到 leader drain 时刻按到达序分配，否则"活区间帧紧凑连续 +
   seq 稠密"不变量（M5 ③ / M6 走读单判据的根基）被破坏。
3. **盘面等价是恢复零改动的前提**：leader 批量写出的字节序列必须与"单写者逐帧
   提交"的盘面逐字节等价（整块重写、撕裂安全、紧凑连续）——M6 恢复链对盘面的
   全部假设因此原样成立，恢复代码与测试零改动。
4. **MPSC 栈取批的逆序问题**：CAS push 得到的是 LIFO 链，leader 必须反转后再
   分配 seq——否则到达序与 seq 序背离，虽然盘上自洽（恢复按 seq 走读），但
   同一写者的先后两帧可能乱序（级别 1 的"先 value 后 WAL"因果可见性存疑），
   反转成本 O(batch) 可忽略，直接做。
5. **leader 在位连任**：leader 写完一批后复查队列，非空则连任再干一批——避免
   "卸任-新选举"的空窗；上限（连任次数/批数）是否需要防饿死，M1 设计稿裁决。
6. **与 P7 的接口承诺**：提交队列就是 ROADMAP P7 "WAL queue endpoint" 的雏形——
   reactor 届时作为写者把帧挂入同一队列，协议无须重做；`WriteWal` 与
   Flush/Reclaim/Close 的跨操作并发互斥届时由 reactor 模型统一裁决。
7. **级别 2/4 为什么不动**：攒批档的 fsync 频率本来就是 1/窗（攒满才刷），
   合并空间已被缓冲吃掉；它们缺的是"多写者并发安全"，那是 P7 的命题。

## 命名与目录约定

沿用 `P<阶段>M<里程碑>_<主题>_design.md`。参见 [doc/P0/README.md](../P0/README.md)。
