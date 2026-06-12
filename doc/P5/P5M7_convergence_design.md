# Cabe P5-M7 设计：P5 收敛

> P5（持久化与崩溃恢复）收口稿。**薄索引形态**（P0M7/P3M4/P4.5M3 先例）：每个里程碑一段摘要 +
> 链回详细设计稿，集中沉淀盘上格式、不变量、错误码与债务流向；不复述各稿正文。
> 本里程碑**零代码改动**（P5M6-D20 钳制条款），收口验证只跑不改。
> **不引入 P6 占位**（P5-D1；对 ROADMAP 衔接约定 #6 的豁免见 §11）。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P5 / M7 |
| 状态 | **✅ 已锁定（P5M7 收敛）** |
| 上游依赖 | P5M1~M6 全部实装 |
| 下游依赖本里程碑 | P6+（以本稿为 P5 终态基准） |
| 退出判定 | 见 §12 |

---

## 1. P5 总览：达成了什么

P5 让 cabe 从"纯内存索引、关机即失忆"走向**真正持久化、崩溃可恢复**：

- 三设备组（数据 / WAL / 快照）经超级块互相锁定，防盘符漂移；
- 每笔写先经 128 字节帧落 WAL（四级持久化可选，默认级别 3），内存索引只是缓存；
- 索引周期性全量镜像到快照设备（A/B 双槽、代际裁决、崩溃任意点旧快照完好）；
- WAL 以环形队列循环复用，快照落地后回收，写满有救援与 `kWalFull` 兜底；
- recover 模式 = 完整恢复链：加载最新快照 + 重放 WAL 活帧 + 重建空闲块，
  启动后状态与崩溃/关闭前一致（级别契约内的丢失除外）。

贯穿 P5 的三条总原则（后续阶段沿用）：**盘上证据为唯一真相，内存皆可重建**；
**部署期校验、运行期不查**；**证据矛盾即拒开，保守方向永远是拒绝服务而非带病上线**。

---

## 2. 里程碑摘要与链回

| 里程碑 | 一句话 | 详细设计 | 决策表 |
|---|---|---|---|
| M1 设备超级块 | 三设备 8K 双份超级块（引擎 UUID + 设备 UUID + 双向配对 + 自身 CRC32C），create/recover 双模式，`RawDevice` 通用裸设备 I/O 工具 | [P5M1_super_block_design.md](P5M1_super_block_design.md) | P5M1-D1~D9 |
| M2 WAL 核心 | `wal/` 模块 + 128 字节帧（seq/双 CRC/key≤84）+ 级别 1 端到端接进 Put/Delete；同步档整块同字节重写（撕裂安全模式，后被 M5 留窗 / M6 重灌与抹除三度复用） | [P5M2_wal_core_design.md](P5M2_wal_core_design.md) | P5M2-D1~D11 |
| M3 WAL 分级 | 级别 2/3/4 + 攒批缓冲（`wal_buffer_size`）+ `Flush()` + 运行时 `SetWalLevel`（收紧先刷净）；默认回到级别 3 | [P5M3_wal_levels_design.md](P5M3_wal_levels_design.md) | P5M3-D1~D10 |
| M4 MetaIndex 快照 | 结构无关 `snapshot/` 模块（回调解耦）；128B 记录 + 4096B 槽头；A/B 对半切写非活跃槽、数据→槽头→一次 fdatasync；`covered_seq` 三方契约；阈值 + 手动双触发与退避 | [P5M4_snapshot_design.md](P5M4_snapshot_design.md) | P5M4-D1~D15 |
| M5 WAL 环形回收 | 模环推进 + 跨缝窗口截短 + 变体 Y 无空洞盘面；头尾指针不持久化（盘上真相 = 帧 + covered_seq）；捕获式回收边界 + `ReclaimUpTo` 三校验；写满救援 + `kWalFull`；容量校验 `ring ≥ max(阈值×2, 缓冲+4K)` | [P5M5_wal_ring_design.md](P5M5_wal_ring_design.md) | P5M5-D1~D18 |
| M6 崩溃恢复 | 九步恢复流水线；快照双槽裁决 + 坏槽回退（seq 连续性兜底）+ `Load`；WAL 两遍扫描 + 走读单判据 + 碎片容差与抹除 + 续写衔接（重灌/稠密续号）；`RebuildFromActive` 增补；守卫拆除全解禁；`verify_value_crc_on_recovery` 兑现 | [P5M6_recovery_design.md](P5M6_recovery_design.md) | P5M6-D1~D20 |
| M7 收敛 | 本稿 | —— | 见前文 M7 设计总结（5 项） |

阶段级决策 P5-D1~D11 见 [doc/P5/README.md](README.md)"已锁定决策"表。

---

## 3. 盘上格式四件套（P5 终态）

| 结构 | 大小 | 位置 | 完整性 | 定义处 |
|---|---|---|---|---|
| `SuperBlock` | 4096 B × 双份 | 三设备 @0 + @4K（数据区一律从 `kDataRegionOffset`=8K 起） | 自身 CRC32C @4092 | `engine/super_block.h` |
| `WalFrame` | 128 B（32 帧/4K 块，不跨块） | WAL 环形区 `[8K, 8K+WalRingSize)` | 帧 CRC32C（自身）+ value CRC32C（读时验） | `wal/wal_frame.h` |
| `SnapshotSlotHeader` | 4096 B | A/B 槽首（`SnapshotSlotSize` 对半切） | 槽头 CRC32C + data_crc（绑死记录流） | `snapshot/snapshot_format.h` |
| `SnapshotRecord` | 128 B（key 区 96 ≥ kWalKeyMax 84） | 槽头之后的记录区 | 整份一条 data_crc（在槽头） | `snapshot/snapshot_format.h` |

几何公式单一来源：`WalRingSize()`（wal.h）/ `SnapshotSlotSize()`（snapshot_format.h）——实现与测试共用，杜绝复抄漂移。仅 Linux/x86_64 native 小端 memcpy，不做跨端序列化。

---

## 4. 四级持久化（终态，含恢复侧语义）

| 级别 | 返回前已完成 | 崩溃丢什么 | 恢复侧形态 |
|---|---|---|---|
| 1 Strict | value FUA + WAL 同步 + 内存 | 不丢 | 全量重建 |
| 2 ValueSync | value FUA + 内存（WAL 攒批） | 未刷出帧的 key 整个消失 | 帧不存在 → 键不在索引、value 成孤立块归空闲（"帧持久 = 承诺成立"）；撕裂刷出的尾碎片被恢复**抹除**（兑现本契约） |
| 3 WalSync（默认） | WAL 同步 + 内存（value 异步） | 键在、值可能坏 | 条目保留，Get 如实报 `kEngineDataCorrupted`；`verify_value_crc_on_recovery` 可提前发现（诊断不改成败） |
| 4 Async | 仅内存 | 未刷出的写整个消失 | 同级别 2 |

不变量（所有级别）：先写内存索引再返回（读己之写）。

---

## 5. 不变量汇总（P5 终态承诺）

**M5 运行期四条**（P5M5 §11）：① 回收绝不越过最新已落地快照的 covered_seq（铁律）；② tail 绝不追上 head（恒留一块）；③ 活区间内帧紧凑连续、无块对齐空洞（变体 Y）；④ seq 严格单调、绕圈不重置。

**M6 恢复期四条**（P5M6 §12）：① 恢复终态 = 某个从未崩溃可达的合法运行态；② 盘上活区间 seq 稠密（走读单判据的对偶，由碎片抹除 + 锚帧+1 续号维护）；③ 恢复对快照/数据设备严格只读，对 WAL 唯一写 = 碎片抹除（有界/幂等/只抹未承诺内容）；④ Open 之后引擎不再记得自己怎么打开的（create/recover 殊途同归零分支）。

推论：环上任意 128B 槽完备三分类（活帧 / 残留 / 无效）；"合法 ∧ seq > covered ⇔ 活帧"判据完备 ⇒ 头尾指针无须持久化、凭扫描推回。

---

## 6. 错误码段位现状（P5 终态）

| 段 | 基址 | 已分配 | 备注 |
|---|---|---|---|
| memory | -100000 | 4 | P0 起 |
| io | -101000 | 0 | 暂以 `kIoBase` 作通用 IO 错，细分码随后端演进补入 |
| index | -102000 | 1 | |
| wal | -103000 | 4 | +M5：`kWalFull`(-103002) / `kWalInvalidReclaim`(-103003) |
| engine | -104000 | 10 | +M6：`kEngineDuplicateBlock`(-104008) / `kEngineBlockOutOfRange`(-104009)；`kEngineNotImplemented` 自 M6 起无人产出（定义保留，只增不删） |
| wal_recovery | -105000 | 13 | 0~9 超级块（M1）；+M6：`kWalRecoveryReadFailed/Corrupted/Invariant`(-105010~12) |
| snapshot | -106000 | 4 | M4 两码 +M6：`kSnapshotReadFailed`(-106002) / `kSnapshotCorrupted`(-106003) |

M6 配码原则立为后续惯例：**码的粒度跟运维动作走（查设备 / 查数据 / 报 bug），诊断细节归日志三件套**。

---

## 7. M6 实装完工对账（P5M7-D1，四笔，余者零偏差）

| # | 偏差 | 处置 |
|---|---|---|
| 1 | `bench/engine/engine_bench.cpp` 写满重开从 recover 改 **create**——M6 后 recover 是真恢复（索引满 + 空闲零 → Put 永远 NoSpace），旧"重置 FreeList"语义随之消亡；设计稿交付范围未预列 | 已实装并注释；P5M6 稿 §1.2 补记（本稿即对账记录） |
| 2 | direct-Wal 测试夹具语义对齐：`MakeWalOpts` 补 `opts.create = true`（`Wal::Open` 分模式后"开空环写入"须显式 create）+ 新增 `MakeWalRecoverOpts` | 已实装；属 §13 测法的实装细化 |
| 3 | 测试矩阵口径：设计 27 例 → 实装 **30 例**——`LoadRejectsBadRecord` 按校验归属拆为快照级内存安全守卫用例（`LoadRejectsOversizeKeyLen`，key_len>96）与引擎级语义校验用例（`RecoverRejectsBadSnapshotRecord`，key_len=0 走统一校验表）；slots 负半边 2 例（越界/重复拒绝）独立计数 | 拆分更贴合"校验分层"设计本身，采纳为准 |
| 4 | 一处测试助手笔误（void 助手误接流式断言），编译期即暴露、当场修正 | 无设计含义 |

除上述四笔，M6 实装与设计稿逐条吻合（D1~D20 全部兑现，§13 矩阵全绿）。

---

## 8. 收口验证实证（P5M7-D3，2026-06-12 于本机）

| 配置 | 结果 |
|---|---|
| Release × sync | **152/152 通过** |
| ASAN × sync | **152/152 通过** |
| TSAN × sync | **152/152 通过** |
| UBSAN × sync | **152/152 通过** |
| Release × io_uring | **162/162 通过**（+io_uring 后端自有用例；TSAN×io_uring 组合按既定排除） |
| 覆盖率 | **总行 86.1% ≥ 80% ✓**（分支 50.8%；`run-coverage.sh` 带三 loop 设备） |

用例构成（sync 152 例）：util/common 基础 51 + RawDevice/超级块/引擎/状态/缓冲池/IO 后端/索引契约 58 + 分配器契约 13（含 M6 负半边 2）+ WAL 35（含 M6 恢复 12）+ 快照 17（含 M6 恢复 8）+ 恢复端到端 8。

**诚实认账（靠审查兜底，无自动化覆盖）**：① I/O 错误注入（loop 设备无故障注入框架，"读错拒开"路径未实跑）；② M6 恢复收尾自检的触发（防自家 bug，无外部构造手段）；③ `last_trigger_seq` 接线与阈值触发行为（无公开观测口，"不为测试扩 API"原则）；④ 撞墙救援端到端（128G 写放大不可行，Wal 级三件套 + 审查覆盖）。

---

## 9. 债务与推迟项流向

| 推迟项 | 流向 | 出处 |
|---|---|---|
| WAL/快照 I/O 异步化（io_uring 化）、后台快照线程、定时刷出（`wal_flush_interval_ms`）、定时快照（`snapshot_interval_sec`） | **P7** | P5-D7 / M3 / M4 |
| TRIM 统一设施（数据盘 `TrimDeviceBlock` + WAL `TrimReclaimedRange` 两处空桩；恢复侧"帧从不被清除"前提届时对账） | **P7** | P4.5 / M5-D15 / M6-D11 |
| 模糊/无锁一致快照（消除冻结写者尖刺）、`covered_seq` 改"已提交水位"、并发恢复 | **P7** | M4-D10/D14 |
| 运行时改 `wal_buffer_size` | 未来 Options 维护接口 | M3 |
| 运维逃生口（强制丢 WAL 按快照恢复等灾难处置） | **P12 `cabe-fsck`** | M6 议题 7.2 |
| 指标接口 | 推迟（P12 Metrics 导出自然承接） | P5-D6 |
| 完整崩溃注入矩阵（子进程 kill -9 / 断电模拟）、I/O 错误注入 | 推迟，单独立项 | P5-D10 / M6 |
| 性能基准与 bench 基线归档 | 发版后补（P5 范围明文；且 M5/M6 两度变更 bench 语义——周期快照开销、create 重开——历史数字可比性已断，现在归档无意义） | P5 范围 / P5M7-D5 |

---

## 10. P5 退出条件核销（doc/P5/README 8 条）

| # | 条件 | 核销 |
|---|---|---|
| 1 | 三设备超级块 + create/recover + 校验 | ✅ M1（M6 起 recover 为完整恢复链） |
| 2 | WAL 模块、128B 帧、四级全生效、默认级别 3 | ✅ M2+M3 |
| 3 | MetaIndex 快照 + 设备布局 + 触发 | ✅ M4 |
| 4 | 环形回收 + 写满兜底（`kWalFull` 救援契约；TRIM 留桩→P7） | ✅ M5 |
| 5 | 崩溃恢复跑通——基础恢复 + 损坏注入测试全绿 | ✅ M6（30 例 + §8 全矩阵） |
| 6 | Engine 集成、Options 扩展、`Engine::Snapshot()` | ✅ M3/M4/M6（恢复类占位字段全部清账，Options 仅余 P7 的两个定时字段存而不用） |
| 7 | P2 文档更新（Options 破坏冻结说明） | ✅ 随各里程碑滚动完成（P2M1 含 M5/M6 两轮冻结追加注） |
| 8 | P5M7 收敛稿审阅通过 + 状态同步 | ✅ 本稿 + §11 |

---

## 11. 状态同步清单与约定豁免

**状态同步**（随本稿一并落）：M1~M6 六份设计稿状态 → "✅ 已锁定（P5M7 收敛）"；`doc/P5/README.md` 状态 → 完成、里程碑表全 ✅、退出条件加核销标记；`ROADMAP.md` P5 段 → "✅ 已实施"；根 `README.md` Roadmap 表 P5 → ✅。

**ROADMAP 衔接约定对账**（6 条）：#1 四档全绿 → §8 实证（CI 仍按 P0M6 决议推迟，本地矩阵代行）；#2 设计稿"已实施"含取舍 → 本稿；**#3 bench 归档 → P5 豁免**（§9 末行，发版后补）；#4 性能红线 → P5 未设红线（首个持久化形态，无可比基线）；#5 README 表同步 → 已列；**#6 下一阶段设计稿启动 → P5 豁免**（P5-D1"收敛稿不引入下一阶段占位"为更具体更晚近的决议；P6 文档随 P6 启动时自建）。

---

## 12. 退出判定

1. ✅ 收敛稿（本稿）审阅通过；
2. ✅ 收口验证全绿实证（§8：四档 × sync 152/152、io_uring 162/162、覆盖率 86.1%）；
3. ✅ 状态同步七处全部落位（§11）；
4. ✅ M6 完工账四笔结清（§7）；
5. ✅ 零代码改动（M7 期间工作区代码 diff 为空，验证构建目录除外）。
