# P5 — 持久化与崩溃恢复 · 设计文档索引

> P5 阶段目标：让 cabe 从"纯内存索引"走向"真正持久化"——实现设备超级块、per-device WAL
> （4 级持久化保证）、快照 + 环形队列空间管理、崩溃恢复。启动时从持久化状态重建内存索引。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P5 — 持久化与崩溃恢复"。

## 状态

✅ **已完成**（M1~M6 全部实装，P5M7 收敛通过；收口实证与债务流向见 [P5M7_convergence_design.md](P5M7_convergence_design.md)）

## 范围摘要

- **设备超级块**：每个数据设备关联 WAL 设备 + 快照设备，三块设备通过超级块（引擎全局 UUID + 设备编号 + 配对 UUID）关联校验；防盘符漂移
- **per-device WAL**：每个数据设备独立 WAL，存放在独立 WAL 裸设备上，仅记录元数据
- **WAL 4 级持久化**（Options 全局配置，默认级别 3）
- **WAL 帧格式**：128 字节固定帧（4096 的因数，32 帧填满一个 4K 块，不跨块），含帧自身 CRC32C + value 的 CRC32C
- **快照 + 环形队列**：MetaIndex 全量镜像写入独立快照设备；WAL 设备以环形队列管理，快照后截断回收（TRIM 实施推 P7，P5M5 留桩）
- **崩溃恢复**：超级块校验 → 加载快照 → 重放 WAL → `BlockAllocator::RebuildFromActive` 重建空闲块
- **启动模式**：仅 create（新建）/ recover（恢复）两种——内存索引易失，每次启动必须重建
- **WAL / 快照 I/O**：同步（io_uring 化推到 P7）；抽取通用裸设备 I/O 工具复用
- **不做**：指标接口（推迟）/ 完整崩溃注入矩阵（推迟）/ 异步 WAL（P7）/ Group Commit（P6）/ 性能基准（发版后补）

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | 设备超级块 | `P5M1_super_block_design.md` | ✅ 已实装 |
| M2 | WAL 模块 + 帧格式 + 严格级别 | `P5M2_wal_core_design.md` | ✅ 已实装 |
| M3 | WAL 分级 2/3/4 + 缓冲区配置 | `P5M3_wal_levels_design.md` | ✅ 已实装 |
| M4 | MetaIndex 快照（检查点） | `P5M4_snapshot_design.md` | ✅ 已实装 |
| M5 | WAL 环形队列回收 | `P5M5_wal_ring_design.md` | ✅ 已实装 |
| M6 | 崩溃恢复 + Engine 集成收尾 | `P5M6_recovery_design.md` | ✅ 已实装 |
| M7 | P5 收敛 | `P5M7_convergence_design.md` | ✅ 收敛通过 |

## 里程碑依赖

```
P5M1 ──► P5M2 ──► P5M3 ──► P5M4 ──► P5M5 ──► P5M6 ──► P5M7
```

严格串行：超级块（基础设施）→ WAL 核心 → WAL 分级 → 快照 → 恢复集成 → 收敛。

## 启动条件

1. ✅ P4.5 全部完成（块分配器抽象层已实装）
2. ✅ P5 决策梳理完成（D1~D11）
3. ✅ 七个里程碑全部经 `/grill-with-docs` 逐项设计并实装完毕

## 已锁定决策（P5 决策梳理）

| 编号 | 决策 | 结论 |
|---|---|---|
| P5-D1 | 里程碑划分 | 6 个里程碑；收敛稿不引入下一阶段占位 |
| P5-D2 | WAL 抽象层 | 不做抽象层，但按 concept 语义度设计接口，降低未来重构成本 |
| P5-D3 | 设备关联 | 三设备超级块关联 + 工程细节（create/recover 区分、超级块自身 CRC、超级块冗余、设备分组、初始化原子性） |
| P5-D4 | WAL 级别 | 四级全做，默认级别 3；所有级别先写内存索引再返回 |
| P5-D5 | 配置归属 | WAL 级别 / 缓冲区大小 / 快照阈值全局统一；破坏 P2 冻结扩展 Options，回头更新文档 |
| P5-D6 | 指标接口 | 推迟，P5 不做（未来补成本低） |
| P5-D7 | WAL/快照 I/O | 同步 I/O（io_uring 推 P7）；抽取通用裸设备 I/O 工具 |
| P5-D8 | 超级块与 block 编号 | bcache 风格：头部 8K 双份超级块，数据区从偏移 8K 起，逻辑 block 从 0；物理偏移 = `kDataRegionOffset + block_idx * kValueSize`，由 IoBackend 加，BlockAllocator 不感知超级块 |
| P5-D9 | 快照协调 | 协调逻辑放 Engine 层；手动接口 `Engine::Snapshot()` |
| P5-D10 | 测试策略 | 基础恢复测试 + WAL 损坏测试；完整崩溃注入矩阵推迟 |
| P5-D11 | 启动模式 + 恢复 | 仅 create/recover；恢复时 value CRC 校验默认关可选；WAL 自管缓冲区（大小 Open 时定死，运行时改大小留未来；`wal_level` 可运行时改） |

## WAL 4 级持久化（D4）

| 级别 | value 落盘 | WAL 落盘 | 内存索引 | 返回时机 | 崩溃代价 |
|---|---|---|---|---|---|
| 1（最严格） | 等待 | 等待 | 写入 | 三者都完成 | 不丢 |
| 2 | 等待 | 异步攒批 | 写入 | value + 内存 | 丢未落盘的 WAL（value 成孤立块，恢复时当空闲） |
| 3（**默认**） | 异步 | 等待 | 写入 | WAL + 内存 | 元数据完整，value 可能损坏（读时 CRC 发现） |
| 4（最宽松） | 异步 | 异步 | 写入 | 仅内存 | 可能丢 value + WAL |

核心不变量：**所有级别都先写内存索引才返回**——保证写入后立即可读（读己之写一致性）。

## 各里程碑范围

### P5M1（设备超级块）

- 三块设备（数据 / WAL / 快照）的超级块格式（含引擎全局 UUID、设备编号、配对 UUID、自身 CRC32C）
- 超级块冗余（头部双份：主 @0 + 备 @4K，bcache 风格）
- 启动模式区分：create（写超级块）/ recover（读超级块校验）
- Open 时校验：引擎 UUID 一致、配对关系正确、数据设备编号与传入顺序匹配
- 设备配置分组结构（`DeviceConfig` 含 data_path / wal_path / snapshot_path）
- 三设备头部 8K 双份超级块，数据区从偏移 8K 起；IoBackend 加数据区偏移（kDataRegionOffset），BlockAllocator 保持从 block 0
- 初始化原子性处理（部分写入的检测与修复）
- 通用裸设备 I/O 工具雏形（打开 + O_DIRECT + 对齐缓冲 + 对齐读写）

### P5M2（WAL 模块 + 帧格式 + 严格级别）

- `wal/` 模块（非抽象层，单个 `Wal` 类，接口按 concept 语义度设计）
- 128 字节固定帧格式：魔数 + 版本 + 类型 + 标志 + **seq（单调序号 / LSN）** + BlockId + timestamp + value CRC + key_len + key（补零，上限 84 字节）+ 帧 CRC32C（精确布局见 `P5M2_wal_core_design.md` §4）
- 级别 1（严格）：value FUA + WAL 同步落盘 + 内存写入，**并端到端接进 `Engine::Put/Delete`**（`DeviceContext` 加 `Wal` 成员）
- WAL 走 `RawDevice`（不经 IoBackend）；线性追加、4K 块整块重写
- 复用 M1 的裸设备 I/O 工具
- 注：M2 只做线性追加、**不判写满**；环形队列（绕圈复用 + 截断回收 + 写满兜底）见 M5

### P5M3（WAL 分级 2/3/4 + 缓冲区配置）

- 级别 2（value 落盘 + WAL 攒批）、级别 3（value 异步 + WAL 落盘，默认）、级别 4（全异步）
- 级别内化：`Engine` 只分发，`Wal`/`IoBackend` 现读 `Options.wal_level` 分支；`Engine::Put/Delete` 不变
- WAL 攒批：一块 `wal_buffer_size` 缓冲、两档共用；新增 `Flush()`；刷出触发 = 攒满 + Close + 切档收紧（**定时刷出 → P7**）
- value 异步（3/4）：`io.Write` 按级别决定 FUA
- Options 的 WAL 字段生效：`wal_level`、`wal_buffer_size` 生效；`wal_flush_interval_ms` 存而不用（P7）
- `wal_buffer_size` Open 时定死、运行期固定（运行时改大小 → 未来）；级别可运行时改（Engine 改级别入口，收紧先刷缓冲）
- 默认级别从 M2 的"强制级别 1"变回级别 3
- 级别 3/4 下 value 损坏的读时检测：复用现有 `Get` 的 CRC；崩溃场景验证在 M6

### P5M4（MetaIndex 快照 / 检查点）

- 新建结构无关的 `snapshot/` 模块（`Snapshot` 类，与 `wal`/`io` 平级）；盘上格式 = 128 字节定长 `SnapshotRecord` + 4096 字节 `SnapshotSlotHeader`
- `MetaIndex` concept 收窄：**移除** `WriteSnapshot`/`LoadSnapshot`（快照 I/O 上移 snapshot 模块），后端只留 `ForEach`（改为返回 `int32_t` 可中止）+ `Insert`
- HashMetaIndex 单线程版快照：`ForEach` 遍历写出（M4 为"冻结写者"的一致快照，无锁/模糊留 P7）
- 快照设备布局：超级块 + 对半切 A/B 双缓冲两槽（每槽 [槽头 4K + 记录数据区]）；槽头记 `generation` + `covered_seq`（覆盖到的 WAL seq）
- 落盘：临时 `snapshot_buffer_size`（默认 1 MiB）缓冲流式写、全程一次 `fdatasync`；快照前 `Wal::Flush()` 刷净 → 取 `covered_seq`
- 触发：大小阈值（主，`snapshot_threshold_bytes`）+ 手动 `Engine::Snapshot()`（辅，同步返结果）；自动触发发后不管；定时（`snapshot_interval_sec`）→ P7
- 原子替换 + 崩溃安全：写非活跃槽、保好覆坏；靠代际号 + 双 CRC 原子切换，任一崩溃点旧快照完好
- 部署期容量约束：Open 时校验**快照设备**容量，不足拒开（`kDeviceTooSmall`）；WAL 设备容量校验设计已定（阈值 × 2~3）、实装推迟 M5（见 P5M4 §11 实装注）
- 注：M4 只做**写流程**；加载快照 + 恢复 → M6；WAL 回收 → M5

### P5M5（WAL 环形队列回收）

- 环形化：日志区 `[8K, 8K+ring_size)` 模环推进、到尾绕回；贴缝窗口截短（不拆分写）；`seq` 绕圈不重置（消歧靠它，帧格式零改动）
- 头尾指针**不持久化**（盘上真相 = 帧 + 快照槽头 covered_seq，指针为运行期缓存）；满/空歧义靠**恒留一块**
- 无空洞盘面（变体 Y）：提前刷出"整块推进、半块留窗"，活区间内帧紧凑连续
- 回收：快照成功落地后，head 跳到**快照定格时刻捕获的物理边界**（covered_seq 的物理孪生）；`ReclaimUpTo` 三条几何校验防倒退/越界；TRIM 留空桩（实施 → P7）
- 写满兜底：空间核算（新起点求值 + 按窗口预留）→ 撞墙强制快照救援 + 重试一次 → 救不了返回 `kWalFull`（-103002）
- 接上 WAL 设备 Open 容量校验：`ring_size ≥ max(snapshot_threshold_bytes × 2, wal_buffer_size + 4K)`（阈值驱动——P5M4 §11 已否定早期"索引条目数 × 2"提法：WAL 无几何硬顶）；既有测试夹具配小阈值（1M / bench 4M）
- 依赖 M4 的快照（检查点定义可回收边界）；解除 M2/M3 "假定 WAL 不写满"；重放/恢复/recover 全归 M6（严格分离）

### P5M6（崩溃恢复 + Engine 集成收尾）

- **九步恢复流水线**（recover 模式 Open）：超级块校验（M1 已有）→ 开数据设备 → 快照双槽裁决（定 covered_seq）→ `Load` 灌索引（基底）→ WAL 两遍扫描（census + 走读，活帧判据 `合法 ∧ seq > covered`）→ 活帧按 seq 序应用（增量压基底）→ 重建 WAL 运行态（重灌留窗 + 锚帧+1 续号）→ `RebuildFromActive`（位图反推；增补越界/重复块报错）→（可选）value CRC 全检
- **失败语义**：要么完整恢复要么干净失败（拒交付半份索引）；证据裁决期间零写盘，唯一写动作 = 撕裂碎片抹除（有界、幂等、只抹级别契约内可丢的帧）；证据矛盾 = 拒开（缺页/碎片越容差/双槽数据皆坏/删不存在键等），保守方向 = 拒绝服务
- **坏槽回退**靠 WAL seq 连续性校验兜底（走读单判据"下一槽 = 下一 seq"，一网封死回退/双坏/双槽无效三个静默丢数据口子）；派生红利：快照纯是加速器——环未绕则创世重放不丢数据
- **终态契约**：Open 之后引擎不再记得自己怎么打开的——recover/create 殊途同归，守卫拆除（`kEngineNotImplemented` 退场）、全机制解禁（阈值触发/手动快照/救援/SetWalLevel 在重建态零新代码成立）
- Options 收尾：`verify_value_crc_on_recovery` 生效（诊断器不是裁判：CRC 不符记日志保条目、不改恢复成败）；错误码 +8（snapshot/wal_recovery/engine 三段）
- 基础恢复测试 + 损坏注入测试 27 例（loop 三设备；优雅闭环 / 盘面篡改 / direct 模块构造三手法；新建 `test/engine/recovery_test.cpp`）
- M6 退出 = P5 退出条件 #1~#6 全满足；**M7 纯文档不碰代码**（钳制条款入稿）

### P5M7（P5 收敛）

- 薄索引收敛稿 + 状态同步（设计稿 / README / ROADMAP）
- 不引入 P6 占位

## P5 退出条件概要

> **全部核销 ✅**——逐条证据见 [P5M7_convergence_design.md](P5M7_convergence_design.md) §10；
> 收口验证（四档检测器 × sync 152/152、io_uring 162/162、覆盖率 86.1%）见其 §8。

1. 三设备超级块格式 + create/recover 启动模式 + 校验逻辑
2. WAL 模块实装，128 字节帧，4 级持久化全部生效，默认级别 3
3. MetaIndex 快照（检查点）+ 快照设备布局 + 触发（M4）
4. WAL 环形队列回收 + 写满兜底（含 `kWalFull` 救援契约；TRIM 留桩、实施 → P7）（M5）
5. 崩溃恢复跑通——基础恢复测试 + WAL 损坏测试全绿（M6）
6. Engine 集成，Options 扩展，`Engine::Snapshot()` 手动接口
7. P2 文档更新（Options 破坏冻结的说明）
8. P5M7 收敛稿审阅通过 + ROADMAP / README 状态同步

## 关键技术备忘（来自决策梳理）

1. **提交顺序随级别变化**：级别 1 = value+WAL+内存；级别 2 = value+内存（WAL 异步）；级别 3 = WAL+内存（value 异步）；级别 4 = 仅内存。
2. **WAL 帧不跨 4K 块**：128 字节是 4096 的因数，32 帧正好填满一个 4K 块——避免跨块读写的复杂性。key 上限 84 字节（加入 seq 字段后的精确值），超长 key 在 Put 时拒绝。
3. **WAL 帧双 CRC 的职责**：帧 CRC32C 校验帧自身完整性（检测不完整写入 + 定恢复边界）；value CRC32C 校验 value 数据（读时校验）。
4. **环形队列 + TRIM**：WAL 设备空间循环使用；快照截断后对释放区域发 TRIM，避免覆盖写时的擦除开销。（P5M5 对账：TRIM **实施推 P7**——M5 在 `ReclaimUpTo` 内留空桩钉死挂点与范围语义，届时与数据盘 `TrimDeviceBlock` 一起经统一 TRIM 设施落地。）
5. **快照不持久化 BlockAllocator**：恢复时从 MetaIndex 反推（位图），毫秒级纯内存操作。（P5M6 兑现：落地为"空闲 = 终态索引的补集"原则——帧持久 = 块有主，帧不存在 = 块归空闲；孤立块六场景全部自然归位。）
6. **恢复时间不是瓶颈**：PB 级 value 的 WAL 仅 GB 级，十几秒可完成；运行时写入延迟才是优化重点。（P5M6 消费：两遍扫描（全环读两遍）与快照两遍读（验 CRC + 投递）的取舍依据即本条。）
7. **模糊快照留待 P7**：M4 单线程哈希表"冻结写者"的一致快照（直接遍历写出；触发快照的那次 Put 阻塞，单线程下"冻结"为语义占位、不上真锁）；P7 多线程 + 模糊/无锁一致快照（哈希表用短锁 + 拷贝，B+ 树用 COW/MVCC）消除延迟尖刺。
8. **WAL 不做抽象层但接口"假装抽象"**：方法语义通用（如 `Append(entry)` / `Sync()`），不暴露裸块设备细节（如 `WriteAt(offset, buf, 4096)`），降低未来引入抽象层的成本。

## 命名与目录约定

沿用 `P<阶段>M<里程碑>_<主题>_design.md`。参见 [doc/P0/README.md](../P0/README.md)。
