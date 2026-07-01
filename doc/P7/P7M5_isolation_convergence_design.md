# Cabe P7-M5 设计:故障隔离 + 收敛(P7 收官)

> 本里程碑是 P7 最后一刀:**故障隔离**(by-construction 论证 + 可控明显故障的轻量演示,不注入运行时
> I/O 故障)+ **收敛**(P7 全量回归全绿 + 收敛稿收口 P7-D1~D13 决策回链、五里程碑、退出条件、与 P11
> 分界)。M5 是 P7 唯一**零产品代码**的里程碑——隔离是 share-nothing 自 M1 起的类型保证、无新机制;
> 主体是几个隔离演示测试 + 本收敛稿 + 文档状态收口 + bench 观察 + 7 配置全量回归。价值是**封口 P7、
> 交出完整绿回归基线**,而非新增功能。
>
> **本文为 M5 详细设计 + P7 收敛索引合一**(薄索引体例,参照 P5M7/P6M3):前半 §1~§4 是 M5 设计,
> 后半 §5~§10 是 P7 整体收口。grill 裁决(A1–A6 + B1–B4)汇集于此。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P7 / M5(收官) |
| 状态 | ✅ **已实装**(P7 收官) |
| 上游依赖 | P7M1~M4 全部完成;P7 阶段计划(doc/P7/README.md,P7-D1~D13、退出条件、与 P11 分界) |
| 下游依赖本里程碑 | 性能兑现轮(流水线 / 并行广播 / group commit 拆壳 / 红线变门槛);P11(真盘大规模验证 + 深度故障隔离 + 运维) |
| 退出判定 | 见 §11 |

---

## 1. 目标与范围

### 1.1 目标

1. **故障隔离论证 + 演示**:by-construction(share-nothing + 跨 device 零通信)论证运行时隔离;可控明显故障
   (坏设备 Open)轻量演示——干净拒开、不污染好设备。
2. **不变量确认**:reactor 停止必唤醒所有等待者(P7-D11 ④;正常 Close 路径 M1 已实现,M5 确认)。
3. **P7 收敛**:全量回归 7 配置全绿 + bench p50 观察 + 收敛稿收口全 P7。

### 1.2 交付范围

| 交付物 | 类别 |
|---|---|
| `test/engine/engine_multidevice_test.cpp` 加 `OpenFailsCleanlyOnBadDevice`(隔离演示) | **唯一的代码**(测试) |
| `doc/P7/P7M5_isolation_convergence_design.md`(本文) | 文档 |
| `doc/P7/P7M1~M4_*.md` 状态 ⏳→✅;`doc/P7/README.md` 状态/里程碑表 | 文档状态 |
| 根 `README.md` P7 行 ⏳→✅;`ROADMAP.md` P7 段状态(项目级,B4 默认纳入) | 文档状态 |
| `bench/baselines/p7/engine.io_uring.gcc.json` + `bench/baselines/README.md` p7 说明 | **留 P11**(M5 未生成,见 §4.1/§10) |

**零改动**:`engine.cpp`/`reactor.cpp` 等**产品代码不动**;**无新增错误码**;无新增 bench 代码(QPS② 推 P11)。

### 1.3 明确不做(各有归处)

| 不做项 | 归处 |
|---|---|
| 部分打开 / per-reactor 降级 / is_dead 标志 + 路由绕开死 reactor | **深度故障隔离 → P11** |
| 运行时 I/O 故障注入 | no-fault-injection(永不;保留防御返回、不测) |
| 异常死亡的优雅降级(`Run` try/catch + 检活) | P11(M5 仅封口为 fail-stop,见 §3.3) |
| 红线② 多线程 QPS 真度量、2 设备 bench fixture | P11(真盘;loop 盘不可信) |
| 两条红线从观察项变退出门槛 | 性能兑现轮(P7-D2) |
| 流水线 / committed_seq / group commit 拆壳 / TRIM / 钉核 | 性能兑现轮(P7-D13) |

---

## 2. 决策汇总

| 编号 | 决策 | 结论 |
|---|---|---|
| **A1** | 隔离本质 | by-construction 论证(share-nothing 自 M1 类型保证),不新增隔离机制 |
| **A2** | 故障注入 | 不注入运行时 I/O 故障(no-fault-injection 贯穿全 P7);深度隔离 → P11 |
| **A3** | 红线② QPS | loop 盘 + 单设备 bench 下不可信 → 观察记一笔、实质推 P11 |
| **A4** | 回归矩阵 | sync 四档(asan/tsan/ubsan/release)+ io_uring 三档(asan/ubsan/release)= 7 配置;TSAN 只在 sync |
| **A5** | 收敛稿形态 | 薄索引(参照 P5M7/P6M3):D1~D13 回链 + 五里程碑 + 退出条件 + 与 P11 分界 |
| **A6** | 覆盖率 | 80% 门槛(`run-coverage --strict`),既有基建,跑一遍记录 |
| **M5-B1** | 交付边界 + S | M5 = 隔离演示测试 + 收敛稿 + 文档状态收口 + bench 观察 + 7 配置回归;**预期零产品代码**(条件于 B2/B3) |
| **M5-B2** | 隔离演示 vs all-or-nothing Open | **保持 all-or-nothing**(合 D11 + S);演示 = by-construction 论证 + 干净拒开测试 + no-orphan;诚实校准 exit① |
| **M5-B3** | 异常死亡 | **不补代码**:半截 try/catch 比 fail-stop 更糟;现行为 = fail-stop 干净响亮;深度降级 → P11 |
| **M5-B4** | 收敛执行 | bench p50 观察(io_uring/gcc 存 p7 基线;QPS② 推 P11)+ 7 配置回归全绿 + 覆盖率 80% + 文档收口;数值跑完回填 |

---

## 3. 故障隔离(M5-B2 + B3)

### 3.1 中心矛盾与解法

README 退出① 说"某设备 Open 失败、**其它继续服务**",但 Open 是 **all-or-nothing**(P7-D11"失败路径最简":
全设备 recover 成功才起 reactor;engine.cpp:88-101 任一 `Start` 失败即 `reactors_.clear()` 全停)。单引擎做不到
部分服务。**保持 all-or-nothing**(改部分打开 = 深度隔离、归 P11,违 D11 + 越 S),把演示重定义为三件:

1. **by-construction 论证**:share-nothing(P7-D4,Reactor 独占 DeviceContext、类型层面保证)+ 跨 device 零通信。
   运行时某设备 I/O 故障,执行体只把错误码经 result 返给路由到它的 caller(`Run` 续跑下个 op、reactor 不退),
   其它 reactor 完全不受影响——**隔离已成立,无需任何主动机制、不注入故障**(no-fault-injection)。
2. **干净拒开测试**(唯一的代码):`Open(2 设备,其一坏路径)` → Open 干净失败(`fail_phase1`/AbortOpen 全部、
   无 fd 漏、无孤儿、返设备错)→ 好设备单独 `Open` + Put/Get 通。证**明显故障被干净处理、不污染好设备,
   好设备可独立服务**。
3. **no-orphan**:reactor 停止唤醒所有等待者(`Stop` 的 `FailWakeChain`)——现有 `OpenGetCloseClean` /
   `DestructorAutoCloses` 已证,不重写。

### 3.2 exit① 诚实校准

all-or-nothing 下"其它继续服务"做不到字面意思。收敛稿据此把 exit① 校准为:**"明显故障(坏设备 Open)被
干净拒开、不污染好设备,好设备可独立打开服务;运行时隔离由 share-nothing by-construction 保证。"**

### 3.3 异常死亡封口(M5-B3)

`Run`(reactor.cpp)无 try/catch。若内部抛异常(如 unordered_map resize 的 `bad_alloc`),线程函数未捕获异常 =
`std::terminate` → 整进程死。这是 P7-D11 预判的"防御角落(no-fault-injection)"。

**不补代码**,理由:加半截 `try/catch` + FailWakeAll **只**救当前批;reactor 线程退出后 Engine 无 `is_dead`
感知,后续路由到该设备的 Submit 压进死 inbox → 那个 caller **永久孤儿**——把"响亮整崩(fail-stop)"换成
"静默局部死 + 后续永久挂",**更糟、更难诊断**。彻底修需 `is_dead` 原子标志 + `SubmitAndWait` 检活 + Engine
路由绕开死 reactor = 真机制、超 S、属深度隔离 → **P11**。

M5 对此角落:确认正常 Close 不变量(§3.1 ③),收敛稿把现行为封口为 **fail-stop(干净响亮失败)**、深度优雅降级
归 P11。

---

## 4. 收敛执行(M5-B4)

> 观察数值与"全绿"确认已由 M5 执行(2026-06-29 本机)回填于下。

### 4.1 bench 观察(红线,P7-D2 观察项、非门槛)

- **红线①(单线程 p50 ≤10% 劣化)**:bench 基建就绪(`run-bench.sh --backend=io_uring` + `bench_engine`,P6 基线在
  `bench/baselines/p6/`)。**M5 未跑形式化 bench**:loop/sparse 盘上数值定性、不可信(README 明示真盘留 P11),
  且 P7 取向不纠结性能(P7-D2)。按论证预期 <10%(OpNode CAS + futex 一跳 µs 级 vs 1MiB I/O 数百 µs)。
  **形式化 p7 基线 + 真盘 p50 度量随 P11**(观察项、非门槛,P7 收官不卡此项)。
- **红线②(多线程 QPS ≥70%×N)**:`bench_engine` 单设备 fixture + loop 盘不可信(A3)→ 不加 bench 代码,真盘度量随 P11。

### 4.2 7 配置全量回归(P7 退出条件 ③,门槛)

| 后端 | 配置 |
|---|---|
| sync | asan / tsan / ubsan / release |
| io_uring | asan / ubsan / release(无 tsan,P4M3/验证策略既定) |

每档 `run-tests.sh --backend=… [--asan/--tsan/--ubsan/--release]` 跑全量 ctest(含多设备),**两组设备齐**
(`mkloop.sh create-multi` 6 块:单设备测试用组 1、多设备用两组)。注:Release 下 M4 的
`assert(dev()==device_id)` 被 NDEBUG 剥离(本就是 debug 防御,无妨)。

**实证(2026-06-29 本机,gcc 15.2.1)**:**全 7 档全绿** —— sync asan/ubsan/release/tsan 各 **179/179**;
io_uring asan/ubsan/release 各 **189/189**(io_uring 多 10 个 backend 用例);sync+TSAN **零 data race**。

### 4.3 覆盖率

`run-coverage.sh --backend=sync --strict` + 两组设备 → 行覆盖率 ≥80%。**实证**:**88.0%**(774/880)≥80% ✓;functions 98.4%、branches 53.5%。

---

## 5. P7 决策回链(D1~D13 收口)

| 编号 | 决策(一句) | 落地里程碑 / 状态 |
|---|---|---|
| D1 | 框架迁移、per-reactor share-nothing、无 mutex、公开 API 冻结 | M1~M4 全程;share-nothing 由 D4 类型保证 |
| D2 | 首轮不纠结性能,两条红线作观察项 | 全程;p50 由 M5 记录(§4.1),QPS 推 P11 |
| D3 | Reactor 放 engine/、普通类、`Start/Stop/Submit`+私有 `Run` | **M1** |
| D4 | Reactor 独占 DeviceContext,share-nothing 成类型保证 | **M1**;M5 隔离论证之基 |
| D5 | OpNode 栈上零堆分配 + MPSC Treiber 栈(无 leader 选举) | M1(Get/Stop)→ M2(写 op 字段)→ M3(真多生产者) |
| D6 | futex 唤醒,唤醒字与 result 分离(避"最后触碰");P7 收敛把唤醒字从 thread_local 改为进程级 WakeSlot 槽以修快路径 UAF(调用线程先退出 → reactor 晚到触碰已析构 thread_local) | **M1**(机制)+ **M5**(收敛修 UAF) |
| D7 | 两级路由 `device=hash%N`,R=1,RouteKey 扁平下标 | M1(返 0 短路)→ **M4**(`RouteToDevice` 真实化 + 三处一致化) |
| D8 | 运营口降格为 op、fan-out、首错聚合;wal_level 转 per-reactor 副本 | M2(per-reactor Options + 顺序广播)→ M4(N>1 真广播);并行广播留性能轮 |
| D9 | 不做显式背压(同步 API 天然有界) | 全程不做 |
| D10 | group commit P7 全程不碰(reactor 单写者批 1) | 全程;拆壳 + 真批量 fdatasync 留性能轮 |
| D11 | 不钉核;Open all-or-nothing;Close drain-then-close;停止必唤醒等待者;异常死亡防御角落 | M1(Close drain-then-close)→ **M5**(隔离确认 + 异常死亡 fail-stop 封口) |
| D12 | 5 里程碑严格串行 | **M1~M5** 全部完成 |
| D13 | 流水线/R>1/TRIM/钉核/io_uring 真异步/group commit 拆壳 全留性能轮 | 全程不碰;见 §8 债务流向 |

---

## 6. P7 五里程碑摘要

| 里程碑 | 交付 | 关键证据 |
|---|---|---|
| **M1** reactor 机制(读路径) | Reactor 类 + MPSC 入站 + 工作线程 + 同步包异步;Get 端到端;DeviceContext 交接 reactor 独占 | Get-miss 经 reactor、drain-then-close、sync+TSAN race-free |
| **M2** 写路径入 reactor | Put/Delete 执行体 + 运营口 + per-reactor Options 副本;写路径辅助迁入 reactor | 39 个写路径用例复活;sync/TSAN/io_uring-ASAN 全过 |
| **M3** 单设备多线程 | 证 reactor 机制 MPSC 正确(零机制改动);多线程并发测试 + 内存序复审(release-sequence) | 6 并发用例 sync+TSAN 零竞争 |
| **M4** 多设备 | 5 处原子翻转(RouteKey/Init/RebuildFromActive/ValidateRecoveredMeta/Open)全用 `super_block.device_id`;运营口真广播 | 4 多设备用例;N=1 向后兼容;TSAN/ASAN 全过 |
| **M5** 隔离 + 收敛 | by-construction 隔离论证 + 干净拒开演示;P7 全量回归 + 收敛稿(本文) | 见 §11 |

---

## 7. P7 退出条件核销

(原文 doc/P7/README.md §P7 退出条件概要 5 条)

1. 数据通路经 reactor(读 M1、写 M2),单调用线程等价旧同步引擎、recover 干净 —— **M1+M2 已证**
2. 多调用线程并发无锁安全(sync+TSAN),§5.3 三竞态消失 —— **M3 已证**
3. 多 device 端到端正确(两级路由、N reactor 并行、运营口 fan-out、各自恢复)—— **M4 已证**
4. 故障隔离:一设备明显故障不连累其它;等待者不孤儿 —— **M5 演示 + 论证**(§3,exit① 诚实校准)
5. P7 全量回归 7 配置全绿 + 收敛稿落定 —— **M5 已证**(§4.2:sync 179×4 / io_uring 189×3 全绿)+ 本文

> 两条红线作观察项、不作门槛(P7-D2):p50① 由 M5 记录;QPS② 推 P11。

---

## 8. 债务流向(P7-D13)

| 债务 | 流向 |
|---|---|
| 流水线 + committed_seq + 跨线程 BufferPool + RWF_DSYNC + 模糊快照 + 同 key 乱序 | 性能兑现轮 |
| group commit 拆壳 + 真批量 fdatasync + WAL 写路径重写 | 性能兑现轮 |
| 运营口并行广播 scatter-gather(`SubmitAndWaitAll`,基础已就位) | 性能兑现轮 |
| TRIM(`reactor.cpp` / `wal.cpp` 两处 TODO) | 性能兑现轮(P7 全程不做) |
| R>1(device 内再分区)、钉核 / NUMA / spin-then-wait、io_uring 真异步 / SQPOLL / DEFER_TASKRUN | 性能兑现轮 |
| 两条红线变退出门槛 | 性能兑现轮 |
| 深度故障隔离(部分打开/降级、is_dead + 路由绕开)、异常死亡优雅降级 | P11 |
| N≥8 真盘大规模、聚合带宽线性度、key 分布偏斜、QPS 真度量、运维文档 | P11 |

产品代码内现存 TODO 仅 2 处(`reactor.cpp` TrimDeviceBlock、`wal.cpp` ReclaimUpTo 后 TRIM),均 P7-D13 明确推后,
**P7 范围内无未了债**。

---

## 9. 与 P11 分界

(原文)P7 = **把多设备做出来**(架构/能力,小 N、loop 设备、正确性优先);P11 = **真盘上大规模验证 + 运维**
(N≥8、聚合带宽线性度、key 分布偏斜、**深度故障隔离**、运维文档),**不新增架构**。多设备的规模/性能度量天然属
P11(真盘上才有意义)。M5 的"深度故障隔离归 P11"(§3.3)、"QPS 真度量归 P11"(§4.1)与此分界一致。

---

## 10. 状态同步清单(M5 执行时落实)

| 文件 | 改动 |
|---|---|
| `doc/P7/P7M1_reactor_skeleton_design.md` §0 状态 | ⏳ 设计中 → ✅ 已实装 |
| `doc/P7/P7M2_write_path_design.md` §0 状态 | ⏳ → ✅ |
| `doc/P7/P7M3_single_device_mt_design.md` §0 状态 | ⏳ → ✅ |
| `doc/P7/P7M4_multi_device_design.md` §0 状态 | ⏳ → ✅ |
| `doc/P7/P7M5_*.md`(本文)§0 状态 | ⏳ → ✅(M5 收尾) |
| `doc/P7/README.md` 整体状态 + 里程碑表 | M1~M5 标完成 |
| 根 `README.md` P7 行 | ⏳ → ✅(项目级,B4 默认纳入) |
| `ROADMAP.md` P7 段 | 加"状态:✅ 已实施"(项目级) |
| `bench/baselines/` | p7 形式化基线随真盘 p50 度量留 P11(M5 未生成,见 §4.1) |

落盘用 cp、不碰 git(沿用 cabe 既有约束)。

---

## 11. M5 退出条件与退出判定

**退出条件**:
1. 明显故障(坏设备 Open)干净拒开、不污染好设备、好设备独立服务(`OpenFailsCleanlyOnBadDevice`)
2. 等待者不孤儿(reactor 停止唤醒全等待者,现有 Close/析构测试已证)
3. P7 全量回归 7 配置全绿(sync 四档 + io_uring 三档;TSAN 在 sync;两组设备)
4. 收敛稿(本文)审阅通过 + §10 状态同步落实
5. (观察,非门槛)p50① 记录;覆盖率 ≥80% 记录

**退出判定(2026-06-29 已核销)**:`OpenFailsCleanlyOnBadDevice` 绿 ✅ + 7 配置全量回归全绿(sync 179×4 /
io_uring 189×3)✅ + 行覆盖率 88.0% ✅ + p50 观察(基建就绪、真盘留 P11)✅ + 状态同步落实(M1~M5 ✅、
doc/P7/README、根 README、ROADMAP)✅ → **P7 收官**。

---

## 12. 关键技术备忘

1. **M5 零产品代码**:隔离是 share-nothing 自 M1 起的类型保证(P7-D4),无新机制;唯一代码是一个干净拒开演示
   用例。收敛是跑回归 + 记数 + 收口文档。
2. **"其它继续服务"诚实校准**:all-or-nothing Open(P7-D11)下做不到字面意思;M5 的隔离 = 明显故障干净拒开不
   污染好设备 + by-construction 运行时隔离论证 + no-orphan。部分打开/降级是深度隔离、归 P11。
3. **异常死亡 fail-stop 优于半截 try/catch**:半截 try/catch 留死 reactor 让后续 op 永久挂、更糟;现行 fail-stop
   是干净响亮失败;优雅降级(is_dead + 路由绕开)归 P11。
4. **红线是观察项**:p50① M5 记录(预期 <10%),QPS② loop 盘不可信、真盘度量归 P11;两者变门槛归性能轮(P7-D2)。
5. **TSAN 证据无空洞**:P7 简单版无 io_uring 专属异步并发面(仍"提交即等待"),并发面全是后端无关的 reactor
   投递/唤醒,sync+TSAN 完整覆盖(验证策略既定)。
