# P5 — 持久化与崩溃恢复 · 设计文档索引

> P5 阶段目标：让 cabe 从"纯内存索引"走向"真正持久化"——实现设备超级块、per-device WAL
> （4 级持久化保证）、快照 + 环形队列空间管理、崩溃恢复。启动时从持久化状态重建内存索引。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P5 — 持久化与崩溃恢复"。

## 状态

🚧 **未启动**（P4.5 全部完成；P5 决策梳理已完成，待开始 M1）

## 范围摘要

- **设备超级块**：每个数据设备关联 WAL 设备 + 快照设备，三块设备通过超级块（引擎全局 UUID + 设备编号 + 配对 UUID）关联校验；防盘符漂移
- **per-device WAL**：每个数据设备独立 WAL，存放在独立 WAL 裸设备上，仅记录元数据
- **WAL 4 级持久化**（Options 全局配置，默认级别 3）
- **WAL 帧格式**：128 字节固定帧（4096 的因数，32 帧填满一个 4K 块，不跨块），含帧自身 CRC32C + value 的 CRC32C
- **快照 + 环形队列**：MetaIndex 全量镜像写入独立快照设备；WAL 设备以环形队列管理，快照后截断回收（TRIM）
- **崩溃恢复**：超级块校验 → 加载快照 → 重放 WAL → `BlockAllocator::RebuildFromActive` 重建空闲块
- **启动模式**：仅 create（新建）/ recover（恢复）两种——内存索引易失，每次启动必须重建
- **WAL / 快照 I/O**：同步（io_uring 化推到 P7）；抽取通用裸设备 I/O 工具复用
- **不做**：指标接口（推迟）/ 完整崩溃注入矩阵（推迟）/ 异步 WAL（P7）/ Group Commit（P6）/ 性能基准（发版后补）

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | 设备超级块 | `P5M1_super_block_design.md` | 待设计 |
| M2 | WAL 模块 + 帧格式 + 严格级别 | `P5M2_wal_core_design.md` | 待设计 |
| M3 | WAL 分级 2/3/4 + 缓冲区配置 | `P5M3_wal_levels_design.md` | 待设计 |
| M4 | MetaIndex 快照实现 | `P5M4_snapshot_design.md` | 待设计 |
| M5 | 崩溃恢复 + Engine 集成 | `P5M5_recovery_design.md` | 待设计 |
| M6 | P5 收敛 | `P5M6_convergence_design.md` | 待设计 |

## 里程碑依赖

```
P5M1 ──► P5M2 ──► P5M3 ──► P5M4 ──► P5M5 ──► P5M6
```

严格串行：超级块（基础设施）→ WAL 核心 → WAL 分级 → 快照 → 恢复集成 → 收敛。

## 启动条件

1. ✅ P4.5 全部完成（块分配器抽象层已实装）
2. ✅ P5 决策梳理完成（D1~D11）
3. ⏳ 用 `/grill-with-docs P5M1` 开第一个里程碑的详细设计

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
| P5-D8 | 超级块与 block 编号 | block 0 作超级块，数据从 block 1 开始，`byte_offset = block_idx * kValueSize` 公式不变 |
| P5-D9 | 快照协调 | 协调逻辑放 Engine 层；手动接口 `Engine::Snapshot()` |
| P5-D10 | 测试策略 | 基础恢复测试 + WAL 损坏测试；完整崩溃注入矩阵推迟 |
| P5-D11 | 启动模式 + 恢复 | 仅 create/recover；恢复时 value CRC 校验默认关可选；WAL 自管缓冲区且运行时可调 |

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
- 超级块冗余（设备头部 + 尾部各一份）
- 启动模式区分：create（写超级块）/ recover（读超级块校验）
- Open 时校验：引擎 UUID 一致、配对关系正确、数据设备编号与传入顺序匹配
- 设备配置分组结构（`DeviceConfig` 含 data_path / wal_path / snapshot_path）
- block 0 作超级块，BlockAllocator Init 从 block 1 开始（修改 P4.5 的代码注释）
- 初始化原子性处理（部分写入的检测与修复）
- 通用裸设备 I/O 工具雏形（打开 + O_DIRECT + 对齐缓冲 + 对齐读写）

### P5M2（WAL 模块 + 帧格式 + 严格级别）

- `wal/` 模块（非抽象层，单个 `Wal` 类，接口按 concept 语义度设计）
- 128 字节固定帧格式：帧头（魔数 + 版本 + 类型 + 标志）+ key_len + key（补零）+ BlockId + value CRC + timestamp + 预留 + 帧 CRC32C
- 级别 1（严格）：value FUA + WAL 同步落盘 + 内存写入
- 环形队列空间管理（头尾指针 + 截断回收）
- 复用 M1 的裸设备 I/O 工具
- WAL 设备容量规划（索引条目数 × 2 倍 + 快满时强制快照兜底）

### P5M3（WAL 分级 2/3/4 + 缓冲区配置）

- 级别 2（value 等 + WAL 攒批）
- 级别 3（value 异步 + WAL 等）——默认级别
- 级别 4（全异步）+ 异步落盘策略（累积量优先 + 定时兜底）
- Options 加 WAL 级别 / 缓冲区大小 / 异步刷出间隔
- WAL 缓冲区大小运行时可调（修改时先刷出当前缓冲）
- 级别 3/4 下 value 损坏的读时 CRC 检测路径

### P5M4（MetaIndex 快照实现）

- `WriteSnapshot` / `LoadSnapshot` 接口参数从文件路径改为裸设备参数
- HashMetaIndex 单线程版快照（直接遍历写出）
- 快照触发：大小阈值（主）+ 手动 `Engine::Snapshot()`（辅）
- 快照设备布局（超级块 + 快照数据区）

### P5M5（崩溃恢复 + Engine 集成）

- 恢复流程：超级块校验 → 加载快照 → 重放 WAL（CRC 校验定边界）→ `BlockAllocator::RebuildFromActive`（位图反推）
- Engine 集成：DeviceContext 加 WAL 成员；Open 区分 create/recover；Put/Delete 路径接入 WAL（按级别）；快照协调
- Options 扩展：设备分组路径 + WAL 级别 + 缓冲区 + 快照阈值 + 恢复时 value CRC 校验开关 + 初始化模式
- 基础恢复测试 + WAL 损坏测试（loop 设备：数据 + WAL + 快照）

### P5M6（P5 收敛）

- 薄索引收敛稿 + 状态同步（设计稿 / README / ROADMAP）
- 不引入 P6 占位

## P5 退出条件概要

1. 三设备超级块格式 + create/recover 启动模式 + 校验逻辑
2. WAL 模块实装，128 字节帧，4 级持久化全部生效，默认级别 3
3. 快照 + 环形队列空间管理 + 截断回收
4. 崩溃恢复跑通——基础恢复测试 + WAL 损坏测试全绿
5. Engine 集成，Options 扩展，`Engine::Snapshot()` 手动接口
6. P2 文档更新（Options 破坏冻结的说明）
7. P5M6 收敛稿审阅通过 + ROADMAP / README 状态同步

## 关键技术备忘（来自决策梳理）

1. **提交顺序随级别变化**：级别 1 = value+WAL+内存；级别 2 = value+内存（WAL 异步）；级别 3 = WAL+内存（value 异步）；级别 4 = 仅内存。
2. **WAL 帧不跨 4K 块**：128 字节是 4096 的因数，32 帧正好填满一个 4K 块——避免跨块读写的复杂性。key 最大约 78~80 字节，超长 key 在 Put 时拒绝。
3. **WAL 帧双 CRC 的职责**：帧 CRC32C 校验帧自身完整性（检测不完整写入 + 定恢复边界）；value CRC32C 校验 value 数据（读时校验）。
4. **环形队列 + TRIM**：WAL 设备空间循环使用；快照截断后对释放区域发 TRIM，避免覆盖写时的擦除开销。
5. **快照不持久化 BlockAllocator**：恢复时从 MetaIndex 反推（位图），毫秒级纯内存操作。
6. **恢复时间不是瓶颈**：PB 级 value 的 WAL 仅 GB 级，十几秒可完成；运行时写入延迟才是优化重点。
7. **模糊快照留待 P7**：P5 单线程哈希表直接遍历写出（触发快照的那次 Put 阻塞，单线程下只影响当前调用者）；P7 多线程 + 模糊快照（哈希表用短暂锁 + swap，B+ 树用 COW）消除延迟尖刺。
8. **WAL 不做抽象层但接口"假装抽象"**：方法语义通用（如 `Append(entry)` / `Sync()`），不暴露裸块设备细节（如 `WriteAt(offset, buf, 4096)`），降低未来引入抽象层的成本。

## 命名与目录约定

沿用 `P<阶段>M<里程碑>_<主题>_design.md`。参见 [doc/P0/README.md](../P0/README.md)。
