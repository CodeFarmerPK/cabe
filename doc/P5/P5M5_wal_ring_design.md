# Cabe P5-M5 设计：WAL 环形队列回收

> 把 WAL 从"线性只增、假定写不满"变成"环形复用、写满有兜底"：日志区到设备尾绕回头部循环
> 复用；快照成功落地后回收已覆盖区段（落实 M4 的"回收与恢复锁步"铁律）；空间耗尽时强制
> 快照救援、救不了用 `kWalFull` 干净拒写。本里程碑**只做写侧**——重放、恢复、头尾指针从
> 盘上重建、recover 模式，全部归 M6（严格分离，本文零提及其设计）。
>
> 延续同步原型基调：单线程、同步实现，不为性能做权衡；正确性（四条不变量）现在就做对。
>
> **本文为详细设计**；C++ 片段为设计示意，代码实装以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P5 / M5 |
| 状态 | **设计稿** |
| 上游依赖 | P5M2/M3（`Wal`：帧 + 缓冲 + 分级 + `Flush`）、P5M4（快照：`covered_seq` + `DoSnapshot` 编排 + 退避） |
| 下游依赖本里程碑 | P5M6（恢复——依赖的是 M5 交付的盘上事实与四条不变量，非本文任何章节） |
| 退出判定 | 见 §16 |

---

## 1. 目标与范围

### 1.1 目标

1. **环形化**：日志区 `[ring_start, ring_end)` 模环推进，到尾绕回；解除 M2/M3 "假定 WAL 不写满"。
2. **无空洞盘面**：攒批提前刷出改为"整块推进、半块留窗"（变体 Y），活区间内帧紧凑连续。
3. **回收**：快照成功落地后，head 跳到快照定格时刻捕获的物理边界——铁律（回收绝不越过最新已落地快照的 `covered_seq`）落地。
4. **写满兜底**：空间核算 + 两档检查点；撞墙时强制快照救援 + 重试一次；救不了返回 `kWalFull`。
5. **部署期容量校验**：`Wal::Open` 校验 `ring_size ≥ max(阈值×2, 缓冲+4K)`（兑现 M4 §11 实装注）。
6. **TRIM 挂点**：`ReclaimUpTo` 内留空桩（实施归 P7，与数据盘 `TrimDeviceBlock` 同待遇）。

### 1.2 交付范围

1. **`wal/wal.h`**（修改）：新成员 `ring_start_`/`ring_end_`/`head_off_`/`window_bytes_`；新接口 `reclaim_boundary()`/`ReclaimUpTo()`/`head_off()`；私有空桩 `TrimReclaimedRange()`；自由函数 `WalRingSize()`（环几何单一来源）；移动构造/赋值带上新成员；模块注释更新为环形形态。
2. **`wal/wal.cpp`**（修改，主战场）：`Open` 算环几何 + 容量校验 + head/窗口初始化；`Append`（同步档）块满推进改模环 + 新起点空间检查；`Flush`（攒批档）变体 Y 整块推进半块留窗 + 模环 + 惰性开窗；`WriteWal` 攒满判定改比有效窗口、无空间返 `kWalFull`；`ReclaimUpTo` 实现。
3. **`engine/engine.{h,cpp}`**（修改，小改）：私有 `WriteWalRescuing`（撞墙救援），`Put`/`Delete` 换调用；`DoSnapshot` 捕获 `boundary` + 成功后 `ReclaimUpTo`。
4. **`common/error_code.h`**（修改）：新增 `kWalFull` / `kWalInvalidReclaim`，段断言更新。
5. **测试**：扩展 `test/wal/wal_test.cpp`（§13 用例矩阵）；夹具阈值改造——`wal_test`/`engine_test`/`snapshot_test` 的 `MakeOpts` 加 `snapshot_threshold_bytes = 1 MiB`，`bench/engine/engine_bench.cpp` 配 4 MiB 并注释语义变化。
6. **关联文档**：新写本稿；历史文档同步（§15）。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| 重放 / 恢复 / 头尾指针从盘上重建 / recover 模式打开 WAL | **P5M6** | 严格分离：M5 只交付"守住四条不变量的环形 WAL"，恢复方案由 M6 自行设计 |
| `Wal` 的任何读盘接口 | P5M6 | M5 的 `Wal` 仍是"只写"模块 |
| TRIM 实施（`BLKDISCARD` 原语 + 统一 TRIM 设施） | **P7** | 纯性能/寿命优化；与数据盘 `TrimDeviceBlock` 空桩同一故事，P7 统一做（value/WAL/快照三层各调） |
| 定时刷出 / 后台快照 / 并发 | P7 | 既有归属不变 |
| 故障注入（快照设备坏时救援自愈等） | 推迟 | 同 M3/M4 先例 |
| "挂起动作对齐窗口边界"类调度优化 | P7 | 讨论中否决于同步原型（挂起状态机 + 契约破坏），异步世界自然复活 |

---

## 2. 现状盘点（M2~M4 给了什么）

- **`Wal`（M2/M3）**：线性追加，`cur_off_` 从 `kDataRegionOffset` 只增不绕；同步档（1/3）当前 4K 块整块重写、块满推进；攒批档（2/4）单块缓冲（`wal_buffer_size` 取整）攒满/Close/切档刷出，`Flush` 按 `AlignUp(used,4K)` 推进（**提前刷出会留块尾补零空洞——M5 以变体 Y 消除**）；不判满，写过设备尾靠 `RawDevice` 返 `kIoBase` 失败安全（**M5 后此路不再可达**）；`seq_next_` 单调；`last_seq()`/`SizeBytes()`（M4 留口）。
- **`Snapshot` + `Engine`（M4）**：`DoSnapshot` = `NoteTriggerAttempt` → `Flush` → 取 `covered_seq` → `Write`；增长触发（阈值）+ 手动 `Snapshot()`；退避基准 `last_trigger_seq` 在 DoSnapshot 入口推进；快照槽头持久 `covered_seq`——铁律的盘上锚点。
- **错误码**：`kWalBase` 段已有 0/1 两号；M2 文档预告 `kWalFull` 留待 M5。
- **`util`**：`AlignUp`/`RoundUpBufferSize` 现成。

---

## 3. 关键决策（已锁定）

| 编号 | 决策 | 结论 |
|---|---|---|
| **P5M5-D1** | 环形几何 | `ring_start = 8K`；`ring_size = 向下取整4K(SizeBytes − 8K)`（向下取整身兼"装得下 + B 端对齐"）；**纯日志区不夹元数据**；Open 算好常驻；公式单一来源 `WalRingSize()` |
| **P5M5-D2** | 绕圈规则 | **模环推进**：恰好到 `ring_end` 即回 `ring_start`（只会恰到、永不越过，可断言）；**seq 绕圈不重置**；"几何能绕"≠"语义允许绕"（后者归写满判定） |
| **P5M5-D3** | 跨缝处理 | **窗口到缝收口（截短不拆分）**：`有效窗口 = min(buf_size, ring_end − cur_off_)`，攒满判定改比有效窗口；否决跨缝拆分写（崩溃形态多一种）；最小窗口 4K 不退化 |
| **P5M5-D4** | 指针持久化 | **头尾指针一律不持久化**，只活内存；盘上真相 = 帧 + 快照槽头 `covered_seq`；安全依据：铁律 ⇒ "合法 ∧ `seq > covered_seq` ⇔ 活帧"判据完备，指针可凭扫描推回；否决 WAL 元数据块（新增持久写 + 撕裂面 + 推翻纯日志区）与槽头捎带（边界规则漏给下游） |
| **P5M5-D5** | 满/空歧义 | **恒留一块**（最多用到 `ring_size − 4K`），`head == tail` 唯一表示空；否决"另记字节数"（冗余状态一致性）；head 永远 4K 对齐；空间核算**按窗口预留** |
| **P5M5-D6** | 盘上形态 | **变体 Y："整块推进、半块留窗"**——提前刷出照写 `AlignUp(used)`，`cur_off_` 只推进整块，半块帧 memmove 回缓冲头续攒、整块同字节重写（M2 撕裂安全模式）；**活区间内帧紧凑连续、无块对齐空洞**；否决变体 X（完全不推进，切档状态对不上）与挂起对齐（状态机 + 契约破坏）；代价：回收推迟 ≤4K、写放大 ≤1 块/次提前刷出 |
| **P5M5-D7** | 回收边界定位 | **快照定格时刻当场捕获**（`Flush` 后的窗口起点，与 `covered_seq` 同刻成对——物理孪生），不做 seq→偏移算术（避免铸造"第 k 帧必在 f(k)"的永久全局不变量）；M5 全程不需要 seq→偏移映射 |
| **P5M5-D8** | 写侧消歧 | 三条件（seq 单调不重置 / 只覆盖已回收区 / 回收不越铁律）⇒ 盘上槽位**完备三分类**（活帧 / 残留 / 无效）；跨圈覆盖撕裂的"混块"被分类收编；**帧格式零改动**（否决 epoch 字段与块级头部） |
| **P5M5-D9** | 回收语义 | **回收 = 一行内存赋值**（head 跳到边界），零盘 I/O 不清零；**铁律 = 调用时机**（仅快照 `Write` 成功后调）；head 模环单调、幂等免特判；不感知 seq |
| **P5M5-D10** | 回收接口 | 三个口：`reclaim_boundary()`（捕获）/`ReclaimUpTo()`（校验 + 赋值）/`head_off()`（观测）；`Wal` 不认识快照；三条纯几何校验（对齐 / 环内 / `[head,tail]` 模环路径——一条式子防倒退防越界，空回收与全量回收取等放行）；失败保守（head 不动）+ 吵闹（FATAL）+ 返回 `kWalInvalidReclaim` |
| **P5M5-D11** | 回收接线 | 内聚 `DoSnapshot`：`Flush` 后同刻捕获 `(covered_seq, boundary)` → `Write` 成功才 `ReclaimUpTo`；**快照成败 = `Write` 成败，回收失败只记 ERROR 不上抛**（快照已落地，报失败是说谎；保守方向零正确性损失）；手动/自动两路自动共用；create 零接线 |
| **P5M5-D12** | 空间核算 | `live(head→新起点) + W ≤ ring_size − 4K`，**在新窗口/新块起点上求值**；同步档块推进时（W=4K）、攒批档开窗时（W=有效窗口）各查一次；**开窗是惰性的**——`Append` 见窗口容量 0 先重试开窗（回收后自然通过），仍失败才 `kWalFull`（救援重试的成立前提） |
| **P5M5-D13** | 救援 | **撞墙反应式**：`WriteWal` 返 `kWalFull` → Engine 强制 `DoSnapshot`（直调、绕过增长闸门——被闸住会卡死且无法自愈）→ **重试一次**（快照成功则数学上必成）；**不设第三个"快满"主动触发**（增长触发管常态、撞墙救援管异常）；双故障下每写一次失败尝试——吵闹换自愈，如实认账 |
| **P5M5-D14** | `kWalFull` 契约 | 对外可见**当且仅当救援无效**；Put 失败 = 新块回收、索引未动、旧值可读；Delete 失败 = 全不动；重试 = 完整重走（被拒帧不在任何缓冲、seq 未耗）；响亮运维信号（根因：快照持续失败/配置病态） |
| **P5M5-D15** | TRIM | 实施归 P7；M5 在 `ReclaimUpTo` 内插**空桩** `TrimReclaimedRange(old_head, new_head)`（`TODO(P7)`：模环区间跨缝拆两段、建议性、失败静默）——挂点与范围语义用代码钉死，与 `TrimDeviceBlock` 同款；统一 TRIM 设施 P7 自行设计，快照设备 TRIM 机会很瘦（A/B 皆承重墙） |
| **P5M5-D16** | 容量校验 | `Wal::Open`（几何后、分配前）：`ring_size ≥ max(snapshot_threshold_bytes × 2, wal_buffer_size + 4K)`，否则 `kDeviceTooSmall`（复用 M4 码，注释当时已含 WAL）；口径用 `ring_size` 非裸 `SizeBytes`（对齐快照侧 `slot_size` 先例）；recover 校验归 M6；日志带三个数 |
| **P5M5-D17** | 改动面 | **零新文件、零新模块、CMake 零改、Options 零新字段**（全复用既有两个字段）；`wal_frame.h`/`snapshot/`/`index/`/`io/` 全零；错误码 +2 |
| **P5M5-D18** | M5/M6 边界 | M5 交付 = 守住四条不变量的环形 WAL；重放/恢复/指针重建/recover/读盘接口全归 M6 且本文零提及其设计；**M5 结束时 recover 行为与 M4 结束时完全相同**（退出判定钉死） |

---

## 4. 环形区盘上几何

```
ring_start = kDataRegionOffset                                   // 8K，双份超级块之后
ring_size  = 向下取整4K( SizeBytes − kDataRegionOffset )         // 环容量，4K 整数倍
ring_end   = ring_start + ring_size                              // 开区间端点 [start, end)
```

```cpp
// wal/wal.h（类外自由函数，与 EncodeFrame 同区）——环几何单一来源，杜绝复抄漂移
inline constexpr std::uint64_t WalRingSize(std::uint64_t device_bytes) noexcept {
    return device_bytes <= kDataRegionOffset
               ? 0
               : ((device_bytes - kDataRegionOffset) / kWalBlockSize) * kWalBlockSize;
}
```

- **向下取整身兼两职**：两槽——两段都装得下（不冲出设备尾）+ 一切推进后偏移仍 4K 对齐；零头留空不用（≤ 4K）。
- **纯日志区**：8K 之后每个字节都是帧槽，不夹任何元数据块（D4 不持久化指针的几何前提）。
- **模环推进**（D2）：两条推进路径（同步档块满、攒批档刷出）各加一处 `next == ring_end ? ring_start : next`；由几何（容量 4K 倍数）+ 跨缝规则（D3）保证 `next ≤ ring_end` 恒成立，可 `assert`。
- **跨缝收口**（D3）：开新窗口时 `window_bytes_ = min(buf_size_, ring_end_ − cur_off_)`；贴缝窗口被临时截短（最小 4K = 32 帧，不退化），攒满恰好顶到环尾 → 绕回 → 恢复满长。内存缓冲仍是 Open 分配的整块，截短的只是本轮用量上限。全部刷出路径（攒满/Close/切档/快照前）共享同一上限，一处截短全员安全。

---

## 5. 头尾指针：表示与持久化

### 5.1 语义

```
tail = cur_off_     // 写尾：下一写出位置（攒批下 = 当前窗口起点）。连续爬行。
head = head_off_    // 回收头：head 之前（模环）已回收可复用。跳跃前进（仅快照落地时跳一步）。
活区间 = [head, tail) 模环（可绕过接缝）
```

- **恒留一块**（D5）：最多用到 `ring_size − 4K` → `head == tail` 唯一表示空。一个 4K 块换掉一整条"冗余计数一致性"不变量。
- head/tail 只是两个已知偏移变量；"追及"是 O(1) 算术检查的安全不变量（一次模减 + 一次比较），与"双指针算法"无关。

### 5.2 不持久化（D4，本里程碑核心盘上契约）

头尾指针**只活在内存**。盘上真相 = **帧本身 + 快照槽头的 `covered_seq`**；指针是可推导的运行期缓存（与快照 `active_slot`/`next_gen` 同性质）。

**安全依据**（"不持久化"决策的论证）：铁律保证被回收空间里残留帧的 seq 必 ≤ 某次已落地快照的 covered_seq ≤ 当前最新值——所以"**校验合法 ∧ `seq > covered_seq` ⇔ 活帧**"是完备判据，指针丢失可凭扫描推回，无须持久化。

否决项：WAL 元数据块（每次回收多一次持久写、元数据块自身成新的撕裂/写序问题、推翻纯日志区——换来的只是省扫已回收段，而恢复时间不是瓶颈）；快照槽头捎带（把"走到哪停"的微妙边界规则漏给下游；槽头 reserved 永在，将来按版本纪律可加，不堵路）。

### 5.3 运行期维护

| 状态 | create 初始值 | 唯一更新点 |
|---|---|---|
| `ring_start_`/`ring_end_` | 按 D1 公式 | 永不（Open 后只读） |
| `cur_off_`（tail） | `ring_start` | 两条既有推进路径（改模环） |
| `window_bytes_` | `min(buf, 全环)` | 开新窗口时重算（含惰性重开，D12） |
| `head_off_`（head） | `ring_start` | 仅 `ReclaimUpTo` |
| `seq_next_` | 1 | 每帧 +1，绕圈不动 |

可用空间**不设成员**、要用现算。create 起步 = 空环（`head == tail == ring_start`），与 M2/M3 现状兼容。recover 模式照旧不开 WAL。

---

## 6. 盘上形态：变体 Y（无空洞）+ 回收边界捕获 + 写侧消歧

### 6.1 变体 Y："整块推进、半块留窗"（D6）

空洞唯一来源 = 攒批档**提前刷出**（Close / 切档收紧 / 快照前 `Flush`——快照一次就可能落一个）。M5 消除之：

```
提前刷出（例：攒了 37 帧 = 1 整块 + 5 帧）：
  盘上:  写 AlignUp(37×128) = 8K（两块都持久,第二块含 5 帧 + 补零）
  推进:  cur_off_ += 4K（只跨过满块）→ 停在半块上
  缓冲:  半块 5 帧 memmove 到缓冲头,n_frames_ = 5,尾部清零
  之后:  新帧从第 6 槽续攒;下次刷出整块同字节重写,补零槽被新帧填上 → 无空洞
```

- 攒满刷出时 `used` 恰为整块 → 留窗部分 = 0，**自动退化为推进语义**——`Flush` 只有一种语义，全部调用方共用。
- **同字节重写 = M2 验证过的撕裂安全模式**；已持久的 covered 帧跨重写仍持久。
- 留窗后状态（`n_frames_ < 32`、帧在缓冲头、`cur_off_` 停在该块）**恰好是同步档"当前块"期望的状态**——切档收紧零额外处理。
- 代价如实记：回收边界推迟 ≤ 4K（留窗块下轮再收）；写放大 ≤ 1 块/次提前刷出；连续两次快照间无写入时留窗块同字节重写一遍（幂等无害，不加脏标记）。
- **Close 留下的是"日志尾半块"，不是洞**（后面没有日志）；跨会话续写归 M6。

### 6.2 回收边界：当场捕获，不做算术（D7）

```
DoSnapshot 定格时刻（Flush 之后）:
    covered_seq = wal.last_seq()
    boundary    = wal.reclaim_boundary()      // = 窗口起点,与 covered_seq 同刻成对(物理孪生)
```

窗口起点之前的一切：① 全是 `seq ≤ covered_seq` 的已覆盖帧；② 永不再被重写（Y 只重写留窗块，在起点之后）——"边界之前 ⇔ 可安全复用"。否决算术反算：它铸造"第 k 帧必在 `f(k)`"的永久全局不变量，将来任何改变落位的事都会无声打破它；捕获按构造为真、零计算零公式。变体 Y 恢复的算术性留作形态红利，M5 正确性不押在它上面。

### 6.3 写侧消歧：三条件 ⇒ 完备三分类（D8）

跨圈覆盖的 4K 写非原子——崩溃后一个块里可能新帧、旧帧、撕裂槽混杂，且两种"合法帧"都过 CRC。三条写侧条件让它无歧义：

① seq 严格单调绕圈不重置（D2）→ 任何两帧可比新旧；② 只覆盖已回收区（D12，tail 不追 head）；③ 回收不越铁律（D9/D11）→ 残留帧 seq 必 ≤ 最新已落地 covered_seq。

```
环上任何 128 字节槽,恰属其一(无第四种):
  合法帧 ∧ seq >  covered_seq   → 活帧
  合法帧 ∧ seq ≤  covered_seq   → 残留(已收编,无害)
  校验不过(补零 / 撕裂)         → 无效(天然跳过)
```

**帧格式一个字节不动**：否决 epoch 字段（seq 单调已是"圈数×圈内位置"合体，冗余）与块级头部（破坏 32 帧整除 4K 布局）——兑现 M2"不加 generation、seq 管消歧"的前瞻。

---

## 7. 回收：语义、接口、接线

### 7.1 语义（D9）

回收 = `head_off_ = boundary`，完。铁律落实在**调用时机**（仅快照 Write 成功后）；head 模环单调、空回收幂等；块对齐自动满足（边界 = 窗口起点）。口语版："一次快照成功落地后，把该快照覆盖范围对应的那段环形区，在记账上标记为可复用。"

### 7.2 接口（D10）

```cpp
std::uint64_t reclaim_boundary() const noexcept { return cur_off_; }  // 捕获口
int32_t       ReclaimUpTo(std::uint64_t boundary);                    // 回收口
std::uint64_t head_off() const noexcept { return head_off_; }         // 观测口
```

校验三条（纯几何）：① `boundary % 4K == 0`；② `ring_start ≤ boundary < ring_end`；③ `(boundary − head) mod ring ≤ (tail − head) mod ring`（一条式子防倒退防越界，取等放行空/全量回收）。失败：head 不动、CABE_LOG_**FATAL**（内部不变式被破坏）、返 `kWalInvalidReclaim`。未打开按校验失败处理（纯防御）。校验通过后、赋值前后调用 `TrimReclaimedRange(old_head, new_head)` 空桩（D15）。

### 7.3 接线（D11）

```cpp
int32_t Engine::DoSnapshot(DeviceContext& dc) {
    dc.snapshot.NoteTriggerAttempt(dc.wal.last_seq());   // 既有(M4)
    int32_t rc = dc.wal.Flush();                          // 既有
    if (rc != err::kSuccess) return rc;
    const std::uint64_t covered_seq = dc.wal.last_seq();          // 既有
    const std::uint64_t boundary    = dc.wal.reclaim_boundary();  // M5 新增(必须在 Flush 后)
    rc = dc.snapshot.Write(covered_seq, ...);             // 既有
    if (rc != err::kSuccess) return rc;                   // 失败 → 不回收,boundary 随栈丢弃
    int32_t rrc = dc.wal.ReclaimUpTo(boundary);           // M5 新增:成功才回收
    if (rrc != err::kSuccess)
        CABE_LOG_ERROR("WAL 回收失败(快照本身已成功,空间暂不复用): rc=%d", rrc);
    return err::kSuccess;                                 // 快照成败 = Write 成败
}
```

回收失败不上抛的理由：只可能是不变量级内部错（已 FATAL+ERROR 双日志），影响是保守方向（空间不复用，最坏 `kWalFull` 早到），而快照真实落了盘——报失败是说谎。**下次快照会捕获新边界再次回收，天然自愈**。create 零接线（head/tail 初始化在 `Wal::Open`）。

---

## 8. 写满兜底与背压

### 8.1 空间核算与检查点（D12）

```
live = (新起点 − head_off_ + ring_size) mod ring_size      // 在新窗口/新块【起点】上求值
获准条件: live + W ≤ ring_size − 4K                        // 4K = 恒留一块
```

| 档 | 检查点 | W | 失败时 |
|---|---|---|---|
| 同步档 | `Append` 块满推进时（对新块起点求值） | 4K | 返 `kWalFull`——帧未编码、**seq 未耗**，干净拒绝 |
| 攒批档 | `Flush` 推进后开新窗口时 | 有效窗口 | 窗口容量记 0；**下一次 `Append` 先惰性重试开窗**（回收后自然通过），仍失败才 `kWalFull` |

- **惰性开窗是救援重试的成立前提**（全量回顾修正项）：否则回收成功后窗口容量仍为 0，重试永远失败。
- 按窗口预留的保守性认账：最多早 ≤ 28K 报满，换一次检查管整窗；环以百 MB 计，无感。
- **背压职责移交**：M2"写过设备尾 → `kIoBase`"的天然兜底在模环推进下不再可达，由本节空间核算取代。

### 8.2 救援（D13）

```cpp
int32_t Engine::WriteWalRescuing(DeviceContext& dc, const WalEntry& e) {
    int32_t rc = dc.wal.WriteWal(e);
    if (rc != err::kWalFull) return rc;              // 正常路径零开销
    CABE_LOG_WARN("WAL 环已满,强制快照腾空间");
    rc = DoSnapshot(dc);                             // 直调:绕过增长闸门
    if (rc != err::kSuccess) return err::kWalFull;
    return dc.wal.WriteWal(e);                       // 重试一次(快照成功则必成)
}
```

- 不设第三个"快满"主动触发：增长触发管常态（容量校验保证它先于撞墙），撞墙救援管异常。
- 绕过闸门的理由：被闸住会卡死（撞墙后写入失败 → WAL 不涨 → 闸门永不重开，设备恢复也无法自愈）；代价（双故障下每写一次失败尝试）如实认账——系统本就在持续报错，吵闹换自愈。
- 重试恰一次：快照成功 → head 跳到窗口起点 → `ring ≥ 缓冲+4K`（D16 下界）保证必成；快照失败 → 循环无意义。
- 救援发生在 Put/Delete 中段，语义自洽：本次 key 未入索引 → 快照不含本次写；被拒帧 seq 未分配 → covered_seq 不含它；重试的新 seq > covered_seq → 活帧。M4"DoSnapshot 不得触发 Put/Delete"不变量不破。

### 8.3 `kWalFull` 契约（D14）

仅救援无效才对外可见——不是"一时紧张"（救援悄悄化解），是"耗尽且救不了"。Put 失败 = 新块回收、索引未动、旧值可读（孤儿 value 写无害，同既有 WriteWal 失败）；Delete 失败 = 全不动；重试 = 完整重走。各级别无特殊：已 ack 的攒批帧必先于撞满刷出。处置：看日志（前面必有成串快照 ERROR）→ 修复后手动 `Snapshot()` 立刻解封（或下次写自愈）→ 部署层加大 WAL / 调低阈值。

---

## 9. TRIM（D15）

实施归 P7。M5 在 `ReclaimUpTo` 内插私有空桩：

```cpp
void Wal::TrimReclaimedRange(std::uint64_t old_head, std::uint64_t new_head) {
    // TODO(P7): 经统一 TRIM 设施对 [old_head, new_head) 模环区间(跨缝拆两段)发建议性
    //   discard(sync=BLKDISCARD / io_uring / SPDK 各后端原语);失败静默,绝不影响回收。
    //   与 Engine::TrimDeviceBlock 同款待遇;统一设施由 P7 自行设计,三层(value/WAL/快照)各调。
    (void)old_head; (void)new_head;
}
```

范围语义与调用时序用代码钉死（P7 只填函数体）；快照设备 TRIM 机会很瘦（A/B 皆承重墙），value 与 WAL 才是大户。

---

## 10. 部署期容量校验（D16）

```
Wal::Open(几何算完后、缓冲分配前):
    ring_size ≥ max( snapshot_threshold_bytes × 2,   // 运转基准:增长触发先于撞墙(M4 论证兑现)
                     wal_buffer_size + 4K )          // 逻辑底线:救援重试必成的数学前提
    不满足 → CABE_LOG_ERROR(带 ring_size 与两条需求) → dev_.Close() → kDeviceTooSmall
```

口径用 `ring_size`（对齐快照侧 `slot_size` 先例；M4 §11 原文 `SizeBytes` 基准，纯精确性细化）。复用 `kDeviceTooSmall`（M4 注释当时已写"快照/ WAL 设备"）。Engine 零改动（既有错误传播 + `AbortOpen`）。recover 校验随 M6。

**对既有测试的破坏性影响（认账）**：默认阈值 512M × 2 = 1G ≫ 16M loop WAL → 设备用例全军覆没。对策：测试夹具显式配 `snapshot_threshold_bytes = 1 MiB`（容量过关：2M ≪ 16M；行为零扰动：触发线 8192 帧 ≫ 既有最大 256 帧）；`engine_bench` 配 4 MiB 并注释"M5 起数字含周期性快照开销"（环形 WAL 带着快照/回收跑才是真实负载）；否决加大镜像（扭曲环境）与校验降级（阉割守卫）。

---

## 11. M5 自身不变量（正确性条款，代码与测试共同守住）

| # | 不变量 | 由谁保证 |
|---|---|---|
| ① | 回收绝不越过最新已落地快照的 `covered_seq`（铁律） | D9/D11 调用时机 |
| ② | tail 绝不追上 head | D12 空间核算 + D5 恒留一块 |
| ③ | **活区间内**帧紧凑连续、无块对齐空洞 | D6 变体 Y + D3 跨缝收口 |
| ④ | seq 严格单调、绕圈不重置 | D2 |

推论（§6.3）：环上槽位完备三分类，跨圈混块无歧义。这四条是 M5 交付物的全部对外承诺。

---

## 12. 错误码 + 改动面

- **错误码 +2**（`kWalBase` 段，段断言更新）：
  ```cpp
  inline constexpr int kWalFull           = InSeg(kWalBase, 2); // -103002  环满且救援无效(运维信号)
  inline constexpr int kWalInvalidReclaim = InSeg(kWalBase, 3); // -103003  回收边界校验不过(内部不变式)
  ```
  不复用 `kWalWriteFailed`——错误码必须说真话（M4 审查 `opts==nullptr` 误报"写失败"的同类教训）。日志分工：`kWalInvalidReclaim` 配 FATAL；`kWalFull` 救援时 WARN、对外时 ERROR。
- **改动面**：`wal/wal.{h,cpp}`（主战场）、`engine/engine.{h,cpp}`（小改）、`common/error_code.h`（+2）、测试三夹具 + bench；**零新文件、零新模块、CMake 零改、Options 零新字段**；`wal_frame.h`/`snapshot/`/`index/`/`io/`/`slots/` 全零。
- **实装备忘**（全量回顾记下，易漏）：① `Wal` 移动构造/赋值带上四个新成员；② 留窗块幂等重写不加脏标记（注释说明）；③ 快照成功但回收校验失败 → 下次快照自然再收（自愈，注释点明）；④ `wal.h` 顶部模块注释更新为环形形态、`wal.cpp` 旧"假定不写满"类注释 grep 清理。

---

## 13. 测试设计

测法基石：**direct-Wal 测试是主力**——`ReclaimUpTo` 直调模拟"快照成功"（`Flush` → `reclaim_boundary()` → `ReclaimUpTo`），摆脱 1 MiB value 写放大（写满 16M 环 = 128K 帧 ≈ 16M 顺序写，秒级）。M3 已有 direct-Wal 先例。

| 用例 | 验证点 |
|---|---|
| `RingGeometry`（不需设备） | `WalRingSize` 公式：常规 / 取整 / 过小设备 → 0 |
| `RingWrapAround` | 写 → 模拟回收 → 续写**越尾绕回**；读盘验环头新帧、seq 连续不重置 |
| `WindowShrinkAtSeam` | 贴缝窗口截短：刷出绝不越 `ring_end`，绕回后恢复满长 |
| `EarlyFlushNoHoles` | 提前 `Flush` → 续写 → 读盘断言**帧紧凑无补零洞**（变体 Y 直接验证） |
| `ReclaimAdvancesHead` | 合法回收 head 前移；空回收幂等；全量回收（==tail）放行 |
| `ReclaimRejectsInvalid` | 倒退 / 越 tail / 不对齐 / 环外 → `kWalInvalidReclaim` + head 不动 |
| `WalFullSyncMode` / `WalFullBatchMode` | 不回收写到满 → 两档各自拒绝点返 `kWalFull`；**恒留一块边界精确**（恰在 `ring−4K` 满） |
| `ReclaimThenWriteSucceeds` | 满 → 回收 → 再写成功（救援数学 + 惰性开窗的 Wal 级验证） |
| 既有全部用例回归 | 阈值 1M 夹具改造后全绿（零行为扰动实证） |

**诚实认账（测不了的）**：① Engine 级"真撞满 + 救援"端到端不可行（128K Put × 1 MiB value = 128G 写放大）——救援语义由 Wal 级三件套全覆盖，`WriteWalRescuing` 胶水靠审查；② Engine 级"快照→回收"接线无黑盒观测口（head 纯内存、Engine 不暴露 `Wal`）——由 direct-Wal 模拟序列覆盖语义，3 行接线靠审查，**不为测试扩公开 API**。

**不测**：恢复/重放（M6）、TRIM 实效（P7 桩）、并发（P7）、故障注入（推迟，同 M3/M4 先例）。落点：全部扩展 `test/wal/wal_test.cpp`。

---

## 14. M5/M6 边界

> **M5 交付：一个守住四条不变量（§11）的环形 WAL。**

重放、恢复、头尾指针从盘上重建、recover 模式打开 WAL、`Wal` 读盘接口——全部归 M6，**本文不预写其设计**（严格分离：可恢复性论证已由 §5.2 承担，恢复方案由 M6 面对盘上事实自行设计）。**M5 结束时，recover 模式的行为与 M4 结束时完全相同**（不开 WAL、不开快照设备、手动快照返回 `kEngineNotImplemented`）——写进退出判定，防实装越界。

---

## 15. 关联文档同步（设计阶段向前同步账单）

| 文档 | 对账 |
|---|---|
| `P5M2_wal_core_design.md` | ① "只增不减(回收在 M5)、假定不写满" → M5 兑现注记；② "写过设备尾 kIoBase 失败安全" → 超越注记（模环后不可达，背压移交空间核算）；③ "`kWalFull` 留待 M5" → 兑现；④ §6 接口注追加 M5（新成员/新接口/Flush 留窗） |
| `P5M3_wal_levels_design.md` | ① §6.5 `Flush` 描述 → 超越注记（M5 起整块推进半块留窗，提前刷出无空洞）；② §6.6 切档措辞对账（刷后可留窗，恰为同步档兼容态）；③ D5"攒满 = 缓冲满" → M5 起以有效窗口为准 |
| `P5M4_snapshot_design.md` | ① §8.3 "M5 据 covered_seq 回收" → 注记：实装为同刻捕获的物理边界（物理孪生）；② §11 实装注 → 兑现 + 口径细化（`ring_size` 基准 + `max(…, 缓冲+4K)`） |
| `doc/P5/README.md` | M5 段按定稿重写；备忘 #4 TRIM → "M5 留桩、P7 统一实施"；退出条件 #4 措辞；状态行与里程碑表（M3/M4 → ✅ 已实装，M5 → 设计稿） |
| `P2M1_api_freeze_design.md` | Put/Delete 承诺表补一行：M5 起可能返回 `kWalFull`（仅空间耗尽且快照救援无效） |
| `ROADMAP.md` | P5 段"快照后截断回收(TRIM)"加小注（TRIM 实施推 P7，M5 留桩） |
| 代码注释 | 实装阶段 grep 收口（`wal.h` 模块注释、"假定不写满"类旧注）——文档阶段不动代码 |

（第二轮对账 = 实装 + 审查修复后的完工回顾，届时结实装偏差的账——M4 先例。）

---

## 16. 退出判定

1. `Wal`：环几何（`WalRingSize` 单一来源）+ 模环推进 + 跨缝窗口截短 + 变体 Y 留窗 + 惰性开窗 + 空间核算（恒留一块）+ `kWalFull` 两档拒绝点 + `reclaim_boundary()`/`ReclaimUpTo()`/`head_off()` + TRIM 空桩 + 容量校验，全部实装。
2. `Engine`：`DoSnapshot` 捕获边界 + 成功后回收（失败不上抛）；`WriteWalRescuing` 撞墙救援接进 Put/Delete。
3. 错误码 `kWalFull`/`kWalInvalidReclaim` 到位，段断言更新。
4. §11 四条不变量全部有测试钉住（§13 矩阵全绿）；既有测试经夹具阈值改造后全绿；bench 注释更新。
5. **M5 结束时 recover 模式行为与 M4 结束时完全相同**（不开 WAL/快照设备，手动快照返 `kEngineNotImplemented`）。
6. Options 零新字段、零新文件、CMake 零改（D17 兑现）。
7. 关联文档同步（§15）完成。
