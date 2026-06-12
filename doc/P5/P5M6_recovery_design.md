# Cabe P5-M6 设计：崩溃恢复 + Engine 集成收尾

> 前五个里程碑全部只做"写侧"，M6 让盘上事实第一次被读回来：recover 模式的 Open 从
> "只校验超级块、空索引"变成完整恢复链——加载最新快照 → 重放 WAL 活帧 → 重建 WAL
> 运行态续写 → 反推空闲块。M5 四条不变量的可恢复性承诺（"指针可凭扫描推回"）在本里
> 程碑全额兑付。M6 是 P5 最后一个功能里程碑：完成后 P5 退出条件 #1~#6 全部满足，
> M7 收敛只剩纯文档。
>
> 延续同步原型基调：单线程、同步实现，恢复时间不是瓶颈（P5 备忘 #6），正确性优先。
>
> **本文为详细设计**；C++ 片段为设计示意，代码实装以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P5 / M6 |
| 状态 | **设计稿** |
| 上游依赖 | P5M1（超级块 recover 校验，已接线）、P5M2/M3（帧格式 + `VerifyFrame` + 级别契约）、P5M4（快照盘上格式 + `covered_seq`）、P5M5（环形 WAL 四不变量 + 完备三分类判据）、P4.5（`RebuildFromActive`） |
| 下游依赖本里程碑 | P5M7（收敛，纯文档）；P7（异步化时复用恢复链结构） |
| 退出判定 | 见 §16 |

---

## 1. 目标与范围

### 1.1 目标

1. **快照加载**：`Snapshot` recover 路径——双槽裁决、data_crc 全量校验、坏槽回退、`Load` 投递、内部状态从盘上重建。
2. **WAL 重放**：`Wal` recover 路径——全环扫描、活帧判据落地、seq 连续性校验（议题 2 托付的安全网）、按 seq 序回调投递、撕裂碎片容差与抹除。
3. **续写衔接**：重放后重建 `Wal` 全部运行态（head/tail/缓冲重灌/续号/窗口），下一次写入与"从未崩溃"逐字节等价。
4. **分配器重建**：终态索引 → `RebuildFromActive`；孤立块自然归位（兑现 P5 备忘 #5 与四级表承诺）。
5. **Engine 集成收尾**：recover 编排总装；M4/M5 守卫拆除、全机制解禁；`verify_value_crc_on_recovery` 兑现（M1 占位字段清账）。
6. **错误码与日志**：恢复段错误码（8 新 + 1 复用）、四级日志分工。

### 1.2 交付范围

1. **`snapshot/snapshot.{h,cpp}`**（修改）：`Open` 增 recover 分支（裁决 + 状态重建）；新增 `Load(const MetaIndexVisitor&)`；`snapshot_format.h` 增 `DecodeSnapshotRecord`。
2. **`wal/wal.{h,cpp}`**（修改，主战场）：`Open` 公共化（recover 下容量校验生效、缓冲推迟分配）；新增 `Recover(covered_seq, WalReplayFn)`（census + 走读投递 + 余环清扫 + 碎片抹除 + 运行态重建 + 收尾自检）；新增 `WalReplayFn` 类型。
3. **`engine/engine.{h,cpp}`**（修改）：私有 `RecoverDevice` / `ApplyRecoveredEntry` / `ApplyWalEntry` / `ValidateRecoveredMeta` / `RebuildAllocator` / `VerifyValuesCrc`；守卫两拆一留；恢复汇总日志。
4. **`slots/ring/ring_block_allocator.cpp`**（修改）：`RebuildFromActive` 增补重复块检测 + 越界报错（concept 零变）。
5. **`common/error_code.h`**（修改）：+8 新码（snapshot 段 2、wal_recovery 段 3、engine 段 2），段断言更新。
6. **测试**：新建 `test/engine/recovery_test.cpp`（`test/CMakeLists.txt` +1 条，M6 改动面含 CMake）；扩展 `wal_test.cpp` / `snapshot_test.cpp`；§13 矩阵 27 例。
7. **关联文档**：新写本稿；历史文档同步（§15）。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| 完整崩溃注入矩阵（子进程 kill -9 / 断电模拟） | 推迟（P5-D10 既定） | 基础恢复 + 损坏注入已覆盖设计面；真崩溃矩阵单独立项 |
| I/O 错误注入测试 | 推迟 | loop 设备无故障注入框架；读错拒开路径靠审查 |
| 运维逃生口（强制丢 WAL 按快照恢复等） | **P12 `cabe-fsck`** | 灾难处置是带人脑判断的离线外科手术，不做 Options 开关（防误操作）；M6 的本分 = 拒得明白，日志即病历 |
| 模糊快照 / 并发恢复 / per-device 并行恢复 | P7/P11 | 既有归属不变；当前 N=1 串行 |
| TRIM（含恢复路径感知 discard 区间） | P7 | §6.2 census 矛盾裁决的前提"帧从不被清除"在 P7 TRIM 实施时需重新对账（见 P5M5-D15 同款前瞻） |

---

## 2. 现状盘点（M1~M5 给了什么）

- **超级块（M1）**：`RecoverDeviceGroup` 已实装且已接进 recover 模式 Open——流水线第 ① 步零新工作。
- **WAL（M2/M3/M5）**：盘上 = 环形纯日志区；`VerifyFrame` 就位（M2 注释明言"供 M6 重放使用"）；四条不变量（铁律 / tail 不追 head / 活区间紧凑 / seq 单调）+ 完备三分类（活帧 / 残留 / 无效）= M6 全部判据的地基；`Wal` 是只写模块，recover 模式不开 WAL 设备。
- **快照（M4）**：A/B 双槽 + 槽头（generation / covered_seq / 双 CRC）；`VerifySlotHeader` 就位；只有写流程，无 Load；M4-D6 代际续号口径（合法头 max+1）等 M6 兑现。
- **分配器（P4.5）**：`RebuildFromActive(dev, block_count, active)` 位图反推，空等三个里程碑。
- **Options**：`verify_value_crc_on_recovery` 占位至今（M1 起存而不用）。
- **错误码**：`kWalRecoveryBase`（-105xxx）段 P0 规划，目前仅超级块占 0~9 号；snapshot 段占 0~1 号。

---

## 3. 关键决策（已锁定）

| 编号 | 决策 | 结论 |
|---|---|---|
| **P5M6-D1** | 恢复流水线 | 九步严格串行：① 超级块（已有）→ ② 开数据设备（已有）→ ③ 开快照设备 + 双槽裁决定 `covered_seq` → ④ Load 灌索引（基底）→ ⑤ 开 WAL + 扫描 → ⑥ 活帧按 seq 序应用（增量压基底）→ ⑦ 重建 WAL 运行态 → ⑧ `RebuildFromActive` → ⑨（可选）value CRC 全检。三条硬依赖：③ 先于 ⑤（covered_seq 是活帧判据的一半）、④ 先于 ⑥（基底先铺、增量后写胜）、⑥⑦ 先于 ⑧（活块集合必须完整）。⑤⑥⑦ 同在 `Wal::Recover` 内；快照与 WAL 两阶段干净隔离不交错 |
| **P5M6-D2** | 职责切分 | 四原则：格式知识不外溢（槽头/记录解析只在 snapshot/，帧校验解码只在 wal/）；索引写入收口 Engine 回调；`covered_seq` 经 Engine 转手、Snapshot 与 Wal 互不相识；MetaIndex / BlockAllocator 全程被动、concept 零改。架构红利：不变量③+④ ⇒ 活区间物理序即 seq 序，**重放免排序** |
| **P5M6-D3** | 失败语义 | 要么完整恢复要么干净失败：任一环错 → `AbortOpen` + 原始码上抛，绝不交付半份索引。**证据裁决期间零盘上写入**；唯一写动作 = 碎片抹除（D13），其后的拒开（⑧/⑨）仅留下"碎片已清零"差异——无害、幂等、不损矛盾证据。"内容少 ≠ 失败"：空槽/空环/空索引是合法终态；证据矛盾才拒开，保守方向 = 拒绝服务。否决两种部分恢复（退回快照态上线 / 跳坏帧拼凑） |
| **P5M6-D4** | 终态契约 | **Open 之后引擎不再记得自己怎么打开的**：recover/create 殊途同归，此后零分支、`options_.create` 无人再读；恢复终态 = 某个从未崩溃可达的合法运行态（§8 自检验收）。`last_trigger_seq = covered_seq`（盘上真实痕迹；肥 WAL 恢复后首写自然触发快照——自愈是推论非特设）。否决恢复即快照（拉长 Open、机制重复）。create 路径字节级零扰动 |
| **P5M6-D5** | 快照 Open 形态 | 单一 `Open(path, opts, data_sb)` 内部按 `opts->create` 分支（签名零改，模式分支下沉格式主人）；容量校验 create/recover 一视同仁（WAL 侧同原则——M5-D16 的 recover 校验在 `Wal::Open` 公共段自然兑现）；recover 不清槽、不修复坏槽头（保全诊断证据） |
| **P5M6-D6** | 槽头裁决 | **I/O 错拒开，内容无效继续**（证据不可得 ≠ 证据说没有；对称 `kSuperBlockReadFailed` 先例）。单槽合法性四条：`VerifySlotHeader` + engine_uuid 比对 + `data_len == entry_count×128` + 槽头+data_len ≤ slot_size（后两条保下游读盘安全）。选槽 = 合法者中代际最大；双合法撞代际 = 矛盾拒开（写侧构造保证不撞号） |
| **P5M6-D7** | 数据校验与回退 | **两遍读**：Open 内对候选槽全量流式验 data_crc（纯读 + CRC，不解码不回调），裁决收口 Open、Load 永不中途反悔——**否决 M4 §7.2 当年的实装建议**（一遍扫边灌边验、不过则清索引回退）：索引污染 + 失败语义搅缠 + 为此扩 `Clear()` 不值。坏槽回退允许但**靠校验保安全非构造保安全**：撕裂（常态，回收未发生，回退安全）与腐烂（罕见，回收已推进，回退有洞）单槽不可区分，安全性交给 WAL seq 连续性校验兜底（D12，跨模块硬依赖）。回退梯子终点 = **双坏拒开不降级**（合法头是"快照发生过"的证据，按 covered=0 假装从未快照是编造历史）——**细化 M4-D8**："都不行则 covered_seq=0" 仅适用于**双槽头皆无效**；合法头 + 双数据坏 = 拒开 |
| **P5M6-D8** | Load 接口 | `int32_t Load(const MetaIndexVisitor&)` 与 `Write(covered_seq, scan)` 合成对称镜像（写注遍历器 / 读注接收器），零新类型、可中止语义一致。无快照空跑成功（Engine 编排直线化）；**空快照（entry_count=0）是合法快照**，与无快照（covered=0）语义有别。流式读临时缓冲（M4"临时分配不走池"延续）；`DecodeSnapshotRecord` 与 Encode 肩并肩入 `snapshot_format.h`；covered_seq 走既有 `last_covered_seq()` 读数口 |
| **P5M6-D9** | 快照状态重建 | 四状态：`active_slot_` = 最终选中槽（回退后 = 回退到的槽）/ 无快照 = -1；**`next_gen_` = 盘上一切合法头的 generation max + 1**（兑现 M4-D6；用"选中槽+1"会让新快照永远输给数据已坏的高代际尸体）；`last_covered_seq_` = 选中槽值 / 0；`last_trigger_seq_` = covered（D4 落点）。写目标"非活跃槽"零新逻辑——保好覆坏跨重启自然延续。双槽头皆无效 → 按"从未快照"恢复，两条世界线由 D12 连续性校验裁决 |
| **P5M6-D10** | 扫描策略 | **两遍扫描**：第一遍普查（census，线性读全环、只看不判、O(1) 内存）+ 第二遍循序走读 + 余环清扫（合计全环两遍，恢复时间不是瓶颈）。单遍否决：活区间跨缝必乱序，保序要么爆内存要么变相两遍。读盘复用 `cur_buf_` 分块读（零新分配零新配置）；扫描中任何读 I/O 失败 = 证据不可得 → 拒开 |
| **P5M6-D11** | 普查 | M5 §6.3 三分类落地（`VerifyFrame` 不过 → 无效；过且 seq > covered → 活帧；否则残留）。采集五标量：`live_count`、`min_live_seq`+偏移、`max_live_seq`+偏移、`max_valid_seq`+偏移（**全局**，无活帧时 = 续写锚）、`valid_count`。census 层裁决一条：全环无合法帧 + covered>0 → 拒开（帧从不被清除；P7 TRIM 实施时此前提需对账）。重复 seq 不单查——由 D13 碎片规则间接拒掉 |
| **P5M6-D12** | 走读与连续性 | 门槛：`live_count==0` 合法（恰好收干净）；`min_live_seq ≠ covered+1` → 拒开（历史开头缺页）。走读单判据 **"下一槽必须恰好是下一 seq"**——一口气盘上验收不变量③（物理紧凑）+④（seq 无跳号）。边走边投递（污染由 D3 全有全无兜底）。停止后与 census 对账：吻合 = 干净收尾快路；不吻合 → 碎片进 D13。派生红利：**快照纯是加速器**——环未绕则创世重放（从 seq 1 全量）精确重建终态，快照全失不丢数据 |
| **P5M6-D13** | 碎片容差与抹除 | 碎片唯一合法来源 = 最后一次撕裂的攒批刷出（fdatasync 屏障 ⇒ 至多一次、必在尾；范围 ≤ 一窗且不跨缝；规则级别无关——跨重启可能换级别）。**双条件容差**：物理 ∈ (停止点, min(停止块 + buf_size_, ring_end))，seq ∈ (T_stop, T_stop + buf_size_/128]；任一不过拒开（与腐烂回退的区分度 = 环级对窗口级）。处置 = **从盘上抹除**（尾块同字节重写 + 容差区余块写零 + 一次 fdatasync；幂等可重入；只在碎片存在时执行、且在全部矛盾裁决通过之后）+ WARN。丢弃正当性 = 级别 2/4 契约白纸黑字允许丢未刷出帧；碎片绝不重放（跳洞是拼凑）。**纠正存照见 §10.3**。两笔认账：跨重启调小 `wal_buffer_size` 可能误拒（补救：按原缓冲重开，零代价）；贴 D16 下界的病态配置区分度退化，不为它加机制 |
| **P5M6-D14** | Wal 接口 | **两段式**：`Open(path, opts)` 管两模式公共段（开设备 + 环几何 + 容量校验），create 分支照旧；新增 `Recover(covered_seq, const WalReplayFn&)` 专职恢复。**缓冲推迟到 Recover 分配**（兼作扫描缓冲 + `cur_buf_==nullptr` 既有守卫天然拒绝过早写入，零新状态位）。`WalReplayFn = (const WalEntry&, std::uint64_t seq) → int32_t`：写侧递 WalEntry 进、重放还 WalEntry 出；Wal 机械解码不解释语义；key 视图仅回调期间有效；回调返错走读立停上抛 |
| **P5M6-D15** | 应用语义 | Put 帧 → `Insert`（有则覆盖）、Delete 帧 → `Delete`，与写路径索引段逐字对仗；同 key 多帧按 seq 序重放后自然留最后结果（last-writer-wins，顺序本身就是全部机制）。分配器全程缺席重放（块流转由终态一次性反推）。**"删不存在的键" = 矛盾拒开**（写侧按构造保证墓碑只在键存在时产生；精确复演下必命中）。ValueMeta 五行直译（block/crc/timestamp 取帧、state 恒 Active）；**timestamp 还原"当年"不用"现在"**（未来 TTL 前提）。语义校验四查（entry_type ∈ {Put,Delete} / key_len ∈ [1, **kWalKeyMax**] / Put 帧 block_idx < block_count / Put 帧 device 位 == 0）四不查（Delete 惰性字段 / timestamp 合理性 / key 字节内容 / seq 重复乱序）；**与快照记录侧合并为同一张校验表、同一份代码**（`ValidateRecoveredMeta`，错误码按来路映射）；违例日志三件套（seq + 偏移 + 哪条不过） |
| **P5M6-D16** | value CRC 全检 | `verify_value_crc_on_recovery` 兑现 = 第 ⑨ 步：对**终态索引**全量 ForEach + 读块 + 比 CRC（否决重放中逐帧查：为注定被覆盖的旧版本读盘 + 对正常历史误报）。**CRC 不符 ≠ 恢复失败**——级别 3 契约下"帧在 value 损"是正常崩溃形态，拒开等于宣布级别 3 不可恢复；处置 = 保留条目（Get 如实报 `kEngineDataCorrupted`，与运行期行为一致；删条目是假装写入没发生）+ 逐键 WARN 设上限 + 汇总 + 恢复成功。读 I/O 错（证据不可得）仍拒开（io 段原始码上抛）。它是诊断器不是裁判；默认关的量级理由：活数据全量读与 GB 级 WAL 不是一个量级 |
| **P5M6-D17** | 续写衔接 | **锚帧** = 有重放集时 T_stop 帧（重放集末帧）/ 无活帧时全局最大合法帧（= covered 帧；此时必无碎片，两定义在各自域内无歧义）/ 空环无锚。`cur_off_` = 锚帧块起点，`n_frames_` = 锚帧槽号+1，块内已有帧**从盘重灌缓冲头**（恢复变体 Y 留窗形态；满块边角推进免灌）；**禁跳块开新**——它在活区间留块对齐空洞，本次无事、下次恢复在洞处停摆（不变量③恢复侧条款）。`head_off_` = 有活帧 AlignDown(min_live, 4K)（构造性等于崩溃前回收头）/ 无活帧 = `cur_off_`（head==tail 表空，D5 延续）。**`seq_next_ = 锚帧 seq + 1`，空环 = 1**（碎片已抹除故复用号段安全；恢复侧新条款：**盘上活区间必须 seq 稠密**，由抹除 + 稠密续号共同维护）。`window_bytes_ = WindowAt(cur_off_)` 一行复用单一来源；满环恢复合法（窗口 0 → 惰性开窗 + 撞墙救援白拿兑现）。派生红利：崩在"快照成功后、回收前" → 丢失的回收被恢复自动补完（M5-D9"回收纯内存"闭环：连执行与否都无需被记住） |
| **P5M6-D18** | 分配器与孤立块 | `ForEach` + `reserve(Size())` 一遍收集活块 → `RebuildFromActive(0, block_count, active)`。**增补**：重复块检测（两键声称同块 = 拒开级矛盾，现状被位图无声吸收）、越界从静默跳过升级为报错（纵深防御第二道闸）；concept 零变，P4.5 文档与既有 slots 测试同步改。总原则：**空闲 = 终态索引的补集**——运行期一切块流转的后果已浓缩在终态；判断分野仅一条：**帧持久 = 承诺成立（块有主），帧不存在 = 承诺未达成（块归空闲）**。六场景验证表见 §9.2 |
| **P5M6-D19** | 错误码与日志 | 配码原则：**粒度跟运维动作走**（查设备 / 查数据 / 报 bug），细节归日志。8 新码 + 1 复用（§11）。日志四级分工：INFO 恢复观察链三行 / WARN 合法非常态 / ERROR 拒开类带三件套 / FATAL 内部不变式；**第一现场原则**：失败只在发现层详记一次，Engine 仅 `AbortOpen` 前兜底一行做索引 |
| **P5M6-D20** | 守卫拆除与 M7 钳制 | 两拆（`Snapshot()` 的 `kEngineNotImplemented` 分支、`MaybeRequestSnapshot` 守卫——M4 成对加成对拆；码保留定义，只增不删纪律）一留（`cur_buf_==nullptr` 守卫升格为时序防御）；grep 清单五处过时注释。五机制过堂（阈值触发 / 手动快照 / 撞墙救援 / SetWalLevel / Close 在重建态零新代码成立）。**M7 钳制条款**：M6 过后 P5 退出条件 #1~#6 全满足，M7 纯文档不碰代码（对称 M5-D18 手法） |

---

## 4. 恢复流水线与编排

### 4.1 九步流水线（D1）

```
Engine::Open(recover)，每设备组：
  ① RecoverDeviceGroup（已有）          超级块三设备校验
  ② io.Open（已有）                     数据设备
  ③ snapshot.Open（recover 分支）       双槽裁决 → covered_seq / active / next_gen / last_trigger
  ④ snapshot.Load(visitor)              快照记录 → 索引（基底）
  ⑤⑥⑦ wal.Open + wal.Recover(covered_seq, fn)
                                        census → 走读投递（增量压基底）→ 清扫 → [抹除] → 运行态重建 → 自检
  ⑧ RebuildAllocator                    终态索引 → 空闲块反推
  ⑨ VerifyValuesCrc（可选，默认关）     纯诊断，CRC 不符不改成败
```

### 4.2 编排代码形态（设计示意）

```cpp
int32_t Engine::RecoverDevice(const DeviceConfig& cfg, DeviceContext& dc) {
    int32_t rc = dc.snapshot.Open(cfg.snapshot_path, &options_, dc.super_block);   // ③
    if (rc != err::kSuccess) return rc;
    rc = dc.snapshot.Load([&](std::string_view k, const ValueMeta& m) {            // ④
        return ApplyRecoveredEntry(dc, k, m);          // 共享校验 + Insert
    });
    if (rc != err::kSuccess) return rc;
    rc = dc.wal.Open(cfg.wal_path, &options_);                                     // ⑤（容量校验在此）
    if (rc != err::kSuccess) return rc;
    rc = dc.wal.Recover(dc.snapshot.last_covered_seq(),                            // ⑤⑥⑦
                        [&](const WalEntry& e, std::uint64_t seq) {
                            return ApplyWalEntry(dc, e, seq);
                        });
    if (rc != err::kSuccess) return rc;
    rc = RebuildAllocator(dc);                                                     // ⑧
    if (rc != err::kSuccess) return rc;
    if (options_.verify_value_crc_on_recovery) {                                   // ⑨
        rc = VerifyValuesCrc(dc);          // CRC 不符返成功+日志；读 I/O 错返原始码（回顾修正 #2）
        if (rc != err::kSuccess) return rc;
    }
    return err::kSuccess;
}
```

`Engine::Open` 仅在既有 `if (opts.create)` 块长出 else 分支调用 `RecoverDevice`；create 内联部分字节级不动。失败统一 `AbortOpen`（既有实现已关三设备）。恢复完成一行 INFO 汇总：

```
恢复完成: 快照[代际 G, N 条, covered_seq=S] + 重放[M 帧, 抹除碎片 K] + 活块[A/总 B]
```

### 4.3 失败语义（D3）与写盘账目

恢复对三设备的写动作**有且仅有一处**：WAL 撕裂尾碎片抹除（D13）。快照设备、数据设备全程严格只读。拒开点与抹除的时序关系（回顾修正 #1 的精确口径）：

| 拒开点 | 相对抹除 | 盘面 |
|---|---|---|
| ③④ 快照裁决/加载、⑤⑥ census/走读/清扫期间 | 之前 | 与 Open 前逐字节相同（证据原封，可反复重试） |
| ⑧ 重复块/越界、⑨ 数据盘读错 | 之后（若有碎片） | 仅差"碎片已清零"——抹的是已裁定的未承诺内容，幂等、不损矛盾证据，重试时走读自然干净收尾 |

---

## 5. 快照加载

### 5.1 Open recover 分支（D5/D6/D7）

```
Open(path, opts, data_sb)：
    开设备 → 算 A/B 布局 → 容量校验            ← 两模式共享（recover 照做：防快照设备被换小）
    create:  清两槽 + sync，active=-1，next_gen=1     ← 既有，零改动
    recover: 读两槽头 → 裁决 → 验数据 → 重建状态      ← M6 新增，全程零写盘
```

裁决梯子（D6/D7）：

```
读 A/B 槽头（任一读 I/O 错 → kSnapshotReadFailed 拒开）
  → 各自过四条合法性（VerifySlotHeader / engine_uuid / data_len 交叉 / 槽界）
  → 双合法撞代际 → kSnapshotCorrupted 拒开
  → 候选 = 合法者中代际最大；无合法头 → "无快照"（covered=0，正常形态）
  → 候选槽数据区流式全量验 data_crc（两遍读的第一遍；读错拒开）
      过   → 选中（常态）
      不过 → ERROR（代际 N 数据损坏）→ 另一合法头槽验数据
              过   → WARN 回退（安全性靠 D12 连续性兜底）
              不过 → kSnapshotCorrupted 拒开（不降级为"无快照"）
```

回退安全性论证（D7 核心）：撕裂形态下（写快照中途崩溃——单次 fdatasync 不保证"数据 → 槽头"的盘上落序，可能留下合法头 + 残缺数据）`Write` 未成功 ⇒ `ReclaimUpTo` 未执行 ⇒ WAL 完整保有老 covered 之后一切，回退即完整恢复；腐烂形态下（落地后位翻转）回收已推进、老快照 + 重放有洞——两者单槽不可区分，由 WAL 侧"min_live == covered+1 且走读稠密"识破洞并拒开。

### 5.2 Load 与状态重建（D8/D9）

`Load` 流式读记录区恰 `entry_count` 条（末块补零不进解码），逐条 `DecodeSnapshotRecord` → visitor 投递；visitor 返错立停上抛。状态重建四项见 D9；`next_gen = 盘上合法头 max+1` 的反例（"选中槽+1"）：回退场景下盘上存在合法的高代际坏槽，低续号的新快照在之后每次恢复中都输给这具尸体——代际必须对**盘上一切合法头**严格单调。

---

## 6. WAL 扫描与定位

### 6.1 两遍扫描（D10）

第一遍 census 线性读全环、逐槽三分类、只记五标量；第二遍从 `min_live` 起循序走读投递、停止后继续清扫余环核对碎片。合计全环读两遍（恢复时间不是瓶颈）；分层价值：第一遍只看不判，裁决集中在两遍之间与走读中，失败日志能精确说出"普查看到什么、裁决在哪不服"。

### 6.2 census（D11）

五标量及消费方：

| 事实 | 消费方 |
|---|---|
| `live_count` | 门槛裁决（D12）：0 = 恰好收干净 |
| `min_live_seq` + 偏移 | 走读起点 + head 重建（D17） |
| `max_live_seq` + 偏移 | 对账（碎片判定输入） |
| `max_valid_seq` + 偏移（全局） | 无活帧时的续写锚 + census 矛盾裁决；有活帧时恒等于 max_live |
| `valid_count` | 诊断 + "全环无帧 ∧ covered>0 拒开" |

### 6.3 走读、门槛与三场景结案（D12）

```
门槛: live==0 → 合法,跳定锚 | min_live==covered+1 → 走读 | 否则 → kWalRecoveryCorrupted
走读: 游标=min_live, 期望=covered+1
      合法帧 ∧ seq==期望 → 投递,期望+1,游标+128(模环)
      其他              → 停(T_stop=期望-1),余环清扫核对碎片(D13)
```

议题 2 托付的安全网结案：

| 场景 | 落点 |
|---|---|
| 坏槽回退·撕裂（常态） | 老 covered+1 起帧全在 → 全程绿，**一字不丢** |
| 坏槽回退·腐烂（被啃中段） | 门槛拒 或 走读断 + 真活区沦为远方碎片 → D13 拒 |
| 双槽头无效·真没快照过 | covered=0、环从未绕（没快照就没回收）→ **创世重放**成功 |
| 双槽头无效·其实有过 | 绕环啃过早期帧 → min_live ≠ 1 拒；环未绕 → 创世重放照样成功 |

### 6.4 碎片容差与抹除（D13）

合法性三事实、双条件、抹除步骤、两笔认账见 D13。抹除时序：定锚重灌（D17）→ 尾块重写（重灌缓冲首 4K 白拿作块映像）→ 容差区余块写零 → fdatasync → 窗口重开 → 自检。**仅在清扫确认存在容差内碎片时执行**；任何矛盾在此前已拒开。

---

## 7. 重放应用（D15/D16）

应用语义、ValueMeta 直译、统一校验表、"删不存在拒开"两步论证（按构造不可能 ⇒ 发生即有东西说谎）、⑨ 步全检——均见 D15/D16。可测试收口性质：**同一键无论经快照还是重放进入索引，恢复后 ValueMeta 与崩溃前逐字段相等**（block/crc/timestamp/state）——"恢复 = 精确复演"的可验证形式（模块级用例验 timestamp，引擎级验值）。

---

## 8. 续写衔接（D17）

### 8.1 六成员重建

```
锚帧块 = AlignDown(锚帧偏移, 4K)
cur_off_   = 锚帧块起点（锚帧在槽 31 时推进到下一块,免灌）
n_frames_  = 锚帧槽号 + 1（块起点到锚帧恰为连续合法帧——含可能的 covered 前缀,即留窗形态本体）
缓冲       = 读回块内 n_frames_×128 字节,其余清零（重灌;同字节重写挂靠 M2/M5 既有撕裂安全论证）
head_off_  = live>0 ? AlignDown(min_live, 4K) : cur_off_      （空环 = ring_start）
seq_next_  = 锚帧 seq + 1（空环 = 1）
window_bytes_ = WindowAt(cur_off_)（单一来源;可为 0 = 满环恢复态,惰性开窗+救援接管）
```

**禁跳块开新**（不变量③恢复侧条款）：跳到下一 4K 边界续写会在活区间留下块对齐空洞——本次无事，下一次恢复走读在洞处停摆，其后已确认的真数据沦为碎片被丢弃甚至拒开。续写位置错一格，下次恢复灾难。

### 8.2 收尾自检（运行期，非 assert——防的是扫描重建代码自身的 bug，Release 不可蒸发）

| 检查 | 守住什么 |
|---|---|
| `head_off_`/`cur_off_` 4K 对齐且在环内 | 几何基线（D5） |
| 几何 live 与走读/census 独立记账的活区块跨度相等 | 定锚对账（回顾修正 #6：原"live ≤ ring−4K"在模环算术下是恒真式，拦不住 bug，替换为本条可证伪检查） |
| `seq_next_ ≥ covered_seq + 1` | 铁律对偶（续号不落回覆盖区） |
| `n_frames_ ≤ 31` 且 `window_bytes_ == WindowAt(cur_off_)` | 留窗/窗口账目 |

不过 → FATAL + `kWalRecoveryInvariant` 拒开（姿态对标 `ReclaimUpTo` 三校验）。

### 8.3 四不变量验收（D4 契约结账单）

| M5 不变量 | 恢复侧由谁保证 |
|---|---|
| ① 回收不越铁律 | head 重建 = 所载快照 covered 的物理孪生重推；恢复全程零回收动作 |
| ② tail 不追 head | 重建只取崩溃前活区间子集（抹除只缩不扩）；回退场景下"老区段完整未被覆盖"在几何上自动蕴含 live ≤ ring−4K（回顾安心项③） |
| ③ 活区间紧凑无洞 | 走读单判据盘上验收 + 重灌 + 抹除 |
| ④ seq 单调 + 稠密（恢复侧新对偶条款） | 走读验收 + 锚帧+1 稠密续号 + 抹除杜绝撞号 |

---

## 9. 分配器重建与 Engine 收尾

### 9.1 RebuildFromActive 增补（D18）

```cpp
// slots/ring/ring_block_allocator.cpp（修改点示意）
if (bid.block_idx() >= block_count) return err::kEngineBlockOutOfRange;  // 原:静默跳过
if (used[bid.block_idx()])          return err::kEngineDuplicateBlock;   // 原:无声吸收
used[bid.block_idx()] = true;
```

### 9.2 孤立块六场景（空闲 = 终态补集）

| # | 场景 | 裁决 |
|---|---|---|
| 1 | 级别 2/4：value 已写、帧没刷出（或沦为碎片被抹） | 键不在索引 → 块归空闲（级别契约：该写从未达成承诺） |
| 2 | 级别 3：帧持久、value 撕裂 | 键**在**索引 → 块保持占用（Get 如实报损；删条目是假装写入没发生） |
| 3 | 覆盖写旧块（含"帧落了、旧块没来得及 Recycle"缝隙） | 终态只引用新块 → 旧块归空闲 |
| 4 | Delete 后的块 | 键不在 → 归空闲 |
| 5 | Put 中途失败 / `kWalFull` 拒绝留下的孤立 value | 无帧 → 归空闲（M5-D14 同一故事） |
| 6 | 仅被残留帧/已抹碎片引用的块 | 不被终态引用 → 归空闲 |

### 9.3 守卫拆除与解禁（D20）

两拆一留 + grep 五处（`engine.cpp` Open / SetWalLevel 注释、`wal.h`/`snapshot.h` 模块注释、`snapshot.cpp` 口径差长注、`wal_frame.h` 预告语）。五机制过堂：阈值触发（last_trigger=covered + last_seq 已重建）、手动快照（DoSnapshot 全链在重建态直接成立）、撞墙救援（满环恢复 → 惰性开窗）、SetWalLevel（收紧 Flush 即常规同字节刷出）、Close（留窗缓冲经既有 Flush 收尾）——零新代码。

---

## 10. 设计过程记录

### 10.1 跨模块安全网（议题 2 → 议题 3 的托付与结案）

"seq 连续性校验"一网兜三处——坏槽回退、双坏拒开、双槽无效——三个静默丢数据的口子由同一条校验封死（§6.3 表）。这条跨模块依赖与 M5"惰性开窗是救援重试的成立前提"同性质：漏掉它，回退机制从安全变成定时炸弹。

### 10.2 组合推演确认项（全量回顾安心项，入档备查）

1. **碎片抹除不会造出"删不存在的键"**：Delete 帧 seq 必大于其针对的 Put；Put 落入碎片区则 Delete 必同在 T_stop 之后——成对丢弃。
2. **腐烂回退后二次崩溃稳定**：高代际坏槽再次落选、走读跨新增帧依旧稠密——多代恢复收敛。
3. **回退场景 live 自动有界**：回退能通过走读的前提（老区段完整未被覆盖）在几何上恰好蕴含 live ≤ ring−4K——恒留一块跨重启自动存续。

### 10.3 纠正存照：续号方向的两难与抹除方案（议题 5.3）

议题 3.5 曾钉死 `seq_next = max_valid + 1`（烧掉碎片号段，防与盘上碎片撞 seq）。议题 5 推演 head/tail/碎片相互作用时发现该方向**错误**，且两个"显然"方案都不可行：

- **烧号**（max_valid+1）：活区间出现永久 seq 跳号 → 下一次恢复走读在断点停摆，其后**已 ack 的新帧**被当碎片吞掉或拒开——这次容忍碎片、下次吞真数据；
- **复用号**（T_stop+1、碎片留盘）：新帧逐块覆盖碎片区，中途再崩 → 盘上残存旧碎片与走读期望 seq 恰好相同 → **借尸还魂**，被丢弃的旧操作混进新历史，静默污染。

两难根源：碎片既不能留在盘上（撞号），它的 seq 又不能不用（断链）。出路 = **把碎片从盘上抹掉，然后安心复用号段**（D13 + D17）。代价 = 给 D3 零写盘原则开一个有界例外。回改四处：3.5 产出表、5.1 锚帧定义（max_valid 帧 → 重放集末帧）、3.4 处置（丢弃 → 抹除）、1.3 零写盘（加例外）。

### 10.4 全量回顾修正清单（六项，已并入正文）

| # | 修正 | 落点 |
|---|---|---|
| 1 | "拒开路径仍零写"收窄为"证据裁决期间零写；抹除后的拒开仅差碎片已清零" | §4.3 |
| 2 | ⑨ 步返回值必须检查（读 I/O 错拒开，CRC 不符返成功） | §4.2 |
| 3 | 统一校验表 key 上限收紧为 `kWalKeyMax`（96 只是字段容量，合法写入恒 ≤84） | D15 |
| 4 | "第二遍只读活区间，放大 <2"改准：走读 + 余环清扫 = 全环两遍 | §6.1 |
| 5 | P4.5 既有 slots 测试若断言越界静默跳过需同步改写 | §13 |
| 6 | 自检第二条 `live ≤ ring−4K` 是模环恒真式 → 替换为"几何 live 与走读跨度对账" | §8.2 |

### 10.5 考虑过并否决的替代方案

- **双坏降级**（双数据坏按 covered=0 创世重放尝试）：理论上安全（连续性校验兜底），但收益是双重稀有事件交集、给"绝不编造历史"凿洞、双槽齐腐时运维该停机调查而非自动续命——否决。
- **恢复即快照**、**一遍扫边灌边验 + Clear 回退**（M4 §7.2 旧建议）、**泄漏 Engine 模拟崩溃**：各见 D4/D7/§13。

---

## 11. 错误码与日志（D19）

```cpp
// snapshot 段（续 2 号）
inline constexpr int kSnapshotReadFailed     = InSeg(kSnapshotBase, 2);    // -106002 槽头/记录区读 I/O 错（查设备）
inline constexpr int kSnapshotCorrupted      = InSeg(kSnapshotBase, 3);    // -106003 撞代际/双坏/记录违例（查数据）
// wal_recovery 段（续 10 号；0~9 为超级块）
inline constexpr int kWalRecoveryReadFailed  = InSeg(kWalRecoveryBase, 10); // -105010 扫描/走读/重灌读 I/O 错（查设备）
inline constexpr int kWalRecoveryCorrupted   = InSeg(kWalRecoveryBase, 11); // -105011 缺页/碎片越界/帧违例/删不存在/无帧但 covered>0（查数据）
inline constexpr int kWalRecoveryInvariant   = InSeg(kWalRecoveryBase, 12); // -105012 收尾自检不过（报 bug）
// engine 段（续 8 号；分配器沿 kEngineNoSpace 先例用本段）
inline constexpr int kEngineDuplicateBlock   = InSeg(kEngineBase, 8);       // -104008 两键声称同块（查数据）
inline constexpr int kEngineBlockOutOfRange  = InSeg(kEngineBase, 9);       // -104009 活块越界（查数据）
```

碎片抹除写失败**复用** `kWalWriteFailed`（码义原文即真话）；⑨ 步数据盘读错走 io 段原始码上抛；"双槽无效"不配码（合法形态非错误）。`kEngineNotImplemented` 保留定义、从此无人产出。

日志：INFO（恢复开始 / 快照裁决 / 完成汇总三行观察链——恢复是 cabe 第一个"把盘上历史读回来"的过程，这三行是行为观察的最小窗口）；WARN（坏槽回退 / 碎片抹除 / value CRC 不符上限+汇总）；ERROR（拒开全员，三件套；候选槽数据坏记 ERROR + 回退动作记 WARN 两级并用；重复块 ERROR 不 FATAL——穿透 CRC 的盘上矛盾不预设是自家 bug）；FATAL（自检）。第一现场原则 + Engine `AbortOpen` 前兜底一行。

---

## 12. M6 自身不变量（与 M5 §11 衔接）

| # | 条款 | 由谁保证 |
|---|---|---|
| ① | 恢复终态 = 某个从未崩溃可达的合法运行态（M5 四不变量全满足） | §8 重建规则 + §8.2 自检 |
| ② | 盘上活区间 seq 稠密（走读单判据的对偶，M5 ④ 的恢复侧延伸） | D13 抹除 + D17 稠密续号 |
| ③ | 恢复对快照/数据设备严格只读；对 WAL 设备唯一写 = 碎片抹除（有界 ≤ 一窗、幂等、只抹未承诺内容） | D3/D13 |
| ④ | Open 之后引擎不再记得自己怎么打开的（recover/create 零分支） | D4/D20 |

---

## 13. 测试设计

**三手法**：① 优雅闭环（create→写→Close→recover→验证；优雅停机是合法恢复输入）；② Close 后 `O_DIRECT`+`pwrite` 盘面篡改（M4 CRC 测试同款，按拒开判定条件反向雕崩溃形态）；③ direct 模块级构造（M5 direct-Wal 延伸；满环/绕圈/碎片边界的唯一高效手段）。否决记档：泄漏 Engine 法（析构自动 Close 绕不开、可被 ③"不 Flush 即重开"等价替代）、子进程 kill -9（P5-D10 推迟）。

**矩阵（27 例）**：

| 层 | 用例（验收点） |
|---|---|
| `wal_test` +12 | RecoverEmptyRing（空环=create 态）/ RecoverLinearBasic（seq 序+字段等值含 timestamp）/ RecoverAfterReclaim（三定锚）/ RecoverWrapAround（跨缝有序）/ RecoverFullRing（窗口 0→kWalFull→回收复活）/ RecoverLeftoverWindow（重灌无洞）/ RecoverThenWriteThenRecover（接缝稠密+二代恢复）/ RecoverFragmentsErased（**抹除直验**：盘上清零+T_stop+1+无还魂）/ RecoverFragmentsRejected（物理/seq 越窗各一）/ RecoverGapRejected（缺页）/ RecoverEmptyRingButCovered（census 矛盾）/ RecoverPrematureWriteGuard（守卫） |
| `snapshot_test` +7 | RecoverPicksMaxGeneration / RecoverSingleValidSlot / RecoverNoSnapshot（-1/1/0+Load 空跑）/ RecoverFallbackOnDataCrc（回退+**next_gen=max+1**）/ RecoverBothDataBad / RecoverGenerationTie / LoadRoundTrip（含空快照）+ LoadRejectsBadRecord（重算 data_crc 的语义违例） |
| `recovery_test` 新建 8 | RecoverBasicRoundTrip / RecoverSnapshotPlusWal（基底+增量跨边界）/ RecoverAfterSlotHeadersWiped（**创世重放红利**）/ RecoverOrphanBlocksReusable（终态补集）/ RecoverRejectsForgedTombstone / RecoverVerifyValueCrcOption（开关双跑、条目保留、Get 报损）/ RecoverTwice / 既有 create 全量零修改回归（零扰动实证） |

slots 既有测试随 `RebuildFromActive` 行为变更同步改写（回顾修正 #5）。

**诚实认账（靠审查）**：I/O 错误注入、§8.2 自检触发、`last_trigger_seq` 接线、救援端到端（M5"不为测试扩 API"原则沿用）。

---

## 14. M6/M7 边界

> **M6 交付：完整恢复链 + 全机制解禁的引擎。**

M6 退出后，P5 退出条件 #1~#6 全部满足；**M7 = 纯文档收敛**（薄索引收敛稿 + 状态同步），不应再有任何代码改动——写进退出判定，防实装越界（对称 M5-D18 钳制 M6 的手法）。

---

## 15. 关联文档同步（设计阶段向前同步账单）

| 类 | 文档 | 对账 |
|---|---|---|
| 实质变更 | `P4.5M1_block_allocator_design.md`（+ P4.5M3/README 提及处） | `RebuildFromActive` 行为变更：重复块/越界报错（原"静默跳过防 UB"注记改写）；"为 P5 恢复服务"兑现 |
| 实质变更 | `P5M4_snapshot_design.md` | D8"都不行则 covered=0"细化（仅双槽头无效；合法头+双数据坏=拒开）；§7.2"一遍扫+Clear 回退"实装建议否决（两遍读取代，`Clear` 留位作废）；§7.1 代际续号兑现；回退安全性补透（依赖 WAL 连续性兜底）；容量校验 recover 照做 |
| 实质变更 | `P2M1_api_freeze_design.md` | `Open` 承诺更新（recover = 完整恢复链）；`Snapshot()` 冻结追加注更新（`kEngineNotImplemented` 退场）；错误码 +8 注记；`verify_value_crc_on_recovery` 生效语义 |
| 兑现注记 | `P5M5_wal_ring_design.md` | §5.2"指针可凭扫描推回"/§6.3 三分类/§13"不测恢复"/§14 边界——M6 兑现；§11 不变量表加恢复侧对偶条款注 |
| 兑现注记 | `P5M2_wal_core_design.md` | `VerifyFrame`/seq"M6 重放定边界"兑现；同字节重写撕裂安全被重灌/抹除复用 |
| 兑现注记 | `P5M3_wal_levels_design.md` | 级别 2/4"崩溃丢未刷出帧"契约的恢复侧扣款（碎片丢弃/抹除的正当性来源）；§9 损坏处置 M6 兑现 |
| 状态索引 | `doc/P5/README.md` | M6 段按定稿重写；里程碑表 M5 → ✅ 已实装（清欠账）、M6 → 设计稿；状态行；备忘 #5/#6 兑现注 |
| 状态索引 | `ROADMAP.md` | P5"崩溃恢复 per-device 并行"加小注（并行为多设备远景，当前 N=1 串行） |
| 代码注释 | grep 清单五处（§9.3） | 实装阶段收口，文档阶段不动代码 |

（第二轮对账 = 实装 + 审查修复后的完工回顾——M4/M5 先例。）

---

## 16. 退出判定

1. `Snapshot` 恢复侧全装：裁决链 + 四状态重建 + `Load` + `DecodeSnapshotRecord`；recover 路径零写盘。
2. `Wal` 恢复侧全装：`Open` 公共化（recover 容量校验）+ `Recover` 全链（census / 门槛 / 走读投递 / 清扫 / 碎片容差 + 抹除 / 六成员重建 / 自检四条）。
3. `Engine` 编排全装：`RecoverDevice` 九步 + 共享校验表一份代码 + 汇总日志；守卫两拆一留 + grep 清净；create 路径字节级零扰动。
4. `RebuildFromActive` 增补到位（重复块 / 越界报错），concept 零变。
5. 错误码 8 新 + 1 复用、段断言更新；日志四级分工 + 第一现场原则落地。
6. Options 零新字段；`verify_value_crc_on_recovery` 生效（恢复类占位字段全部清账）。
7. §13 矩阵 27 例全绿 + 既有全量用例零修改全绿；slots 测试随行为变更改写；四项认账记档。
8. §15 账单三类全部结清；本稿含纠正存照（§10.3）与回顾修正（§10.4）。
9. 终态契约可验证：五机制过堂零新代码成立；M5 四不变量 + 恢复侧稠密条款跨重启存续（§12）。
10. **M7 钳制**：M6 过后 P5 退出条件 #1~#6 全满足，M7 纯文档不碰代码。
