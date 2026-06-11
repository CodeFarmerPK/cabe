# Cabe P5-M4 设计：MetaIndex 快照（检查点）

> 把内存里的 `MetaIndex`（key → `ValueMeta`）全量落到独立快照设备上，形成一份**检查点**。
> 恢复（M6）= 加载最新快照 + 重放快照之后的 WAL 尾；回收（M5）凭快照记录的 `covered_seq`
> 截断 WAL。本里程碑**只做"写一份快照"这条路**——格式、设备布局、双缓冲两槽、触发、
> 原子替换与崩溃安全；**加载/恢复留 M6，WAL 回收留 M5，后台/异步/定时留 P7**。
>
> 延续 M3 的同步原型基调：单线程、同步实现，**不为性能做权衡**（延迟尖峰、并发一致性
> 全部留 P7 异步化）；正确性（崩溃安全、数据一致）现在就做对。
>
> **本文为详细设计**；C++ 片段为设计示意，代码实装以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P5 / M4 |
| 状态 | **设计稿** |
| 上游依赖 | P5M1（超级块 + `RawDevice` + create/recover + `DeviceContext`）、P5M2/M3（WAL 帧 + `seq` + 公开 `Flush()` + `Options`） |
| 下游依赖本里程碑 | P5M5（环形回收：凭快照 `covered_seq` 定回收边界）、P5M6（恢复：加载快照 + 从 `covered_seq+1` 重放 WAL） |
| 退出判定 | 见 §16 |

---

## 1. 目标与范围

### 1.1 目标

1. 定义快照**盘上格式**：128 字节定长 `SnapshotRecord` + 4096 字节 `SnapshotSlotHeader`。
2. 新建**结构无关的 `snapshot/` 模块**（`Snapshot` 类），与 `wal/`、`io/` 平级——三设备三管理模块。
3. **快照设备布局**：8K 双份超级块（M1 已有）+ 对半切 **A/B 双缓冲两槽**；每槽 `[槽头 4K][记录数据区]`。
4. **写一份快照**：遍历内存索引（`ForEach`）→ 过一块临时缓冲流式写非活跃槽 → 写槽头 → 一次 `fdatasync`。
5. **双缓冲选新 + 崩溃安全**：写非恢复槽、保好覆坏；靠**代际号 + 双 CRC** 做原子切换，任一崩溃点旧快照完好。
6. **与 WAL 衔接**：每份快照记录 `covered_seq`（覆盖到的最大 WAL `seq`），作为 M5/M6 的唯一契约。
7. **触发**：手动 `Engine::Snapshot()`（同步、返回结果）+ 大小阈值（自动、fire-and-forget）；汇总到一个 `RequestSnapshot()` 入口。
8. **部署期容量约束**：Open 时校验快照设备 / WAL 设备容量；不足拒绝打开。

### 1.2 交付范围

1. **`snapshot/snapshot_format.h`**（新建）：`SnapshotRecord`（128B）+ `SnapshotSlotHeader`（4096B）+ 常量（`kSnapshotRecordSize` / `kSnapshotKeyMax` / `kSnapshotSlotMagic` 等）+ 单条/槽头的编码、解码、校验自由函数。
2. **`snapshot/snapshot.{h,cpp}`**（新建）：`Snapshot` 类（持快照设备句柄 + A/B 布局 + 代际 + active_slot）；`Open` / `Write` / `Close`（`Load` 留 M6）。
3. **`engine/device_context.h`**（修改）：加 `Snapshot snapshot;` 成员（`wal` 旁）。
4. **`engine/engine.{h,cpp}`**（修改）：`Open`（create 模式开快照设备 + 算布局 + 容量校验 + 清空槽 + `next_gen=1`）；新增 `Engine::Snapshot()` + 内部 `RequestSnapshot` / `MaybeRequestSnapshot` / `DoSnapshot`；`Put`/`Delete` 收尾插触发检查；`Close` 加 `snapshot.Close()`。
5. **`wal/wal.{h,cpp}`**（修改）：加只读 `last_seq()`（返回 `seq_next_ - 1`）与 `SizeBytes()`（M5 容量校验用）；空 Options 错误码统一为 `kEngineInvalidOpts`、缓冲规整改用共享函数（见第 11 条）。
6. **`index/meta_index.h` + `index/hash/hash_meta_index.{h,cpp}`**（修改）：concept **收窄**——移除 `WriteSnapshot`/`LoadSnapshot`；`ForEach` 改为**返回 `int32_t`、可中止**（`MetaIndexVisitor` 也返回 `int32_t`）；同步改那唯一一处 `ForEach` 测试。
7. **`engine/options.h`**（修改）：新增 `snapshot_buffer_size`（默认 1 MiB）；订正快照配置注释（阈值 M4 生效、定时 P7、缓冲 M4 生效）。
8. **`common/error_code.h`**（修改）：新增 `kSnapshotWriteFailed` / `kDeviceTooSmall`。
9. **测试**：新建 `test/snapshot/`（格式往返 + 端到端写快照读盘核对 + 解码逐字段比对 + A/B 选槽 + 代际 + 触发正/负半边 + 容量校验拒绝 + `ForEach` 可中止）。
10. **关联文档**：新写本稿；同步历史文档（§15）。
11. **`util/crc32.{h,cpp}`**（修改，实装新增）：加软件路径的**流式增量 CRC32C** `CRC32CStreamUpdate(crc, data)`（复用同一张表，与 `CRC32` 结果一致）——流式写快照分块累积 `data_crc` 必需，现有一次性 `CRC32` 不够用。
12. **`util/util.h`**（修改，实装新增）：加 `RoundUpBufferSize(size, block)`（钳 ≥block 并向上取整），`Wal::Open` 与 `Snapshot::Write` 共用，消除两处重复。
13. **`test/common/test_env.h`**（新建，审查修复轮）：共享测试工具 `GetEnv`，收敛 wal/engine/super_block/snapshot 四个测试文件的逐字拷贝（bench 保留自己的副本——独立目标）。

> 注：1~9 的代码改动按工作流在"写代码"阶段落地，11~13 为实装/审查修复轮补充；本设计稿先行。Open 时容量校验依赖快照设备先打开（本里程碑引入）。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| 加载快照 `Load` + 完整恢复编排（recover 模式 Open + 重放 WAL + `RebuildFromActive`） | **P5M6** | M4 只做写流程；恢复是 M6 |
| WAL 环形回收 / 截断 TRIM / 写满兜底 | **P5M5** | M4 的 WAL 仍线性只增，回收凭快照 `covered_seq`（M5）。（P5M5 注：环形回收与写满兜底已设计落定；**TRIM 实施再推 P7**——M5 仅在 `ReclaimUpTo` 内留空桩钉挂点） |
| 定时触发（`snapshot_interval_sec` 生效） | **P7** | 定时要后台线程；定时的真正归宿是 P7 后台快照 |
| 后台 / 异步快照（非阻塞）、并发合并、真实锁 | P7 | 单线程下快照同步内联；无并发 |
| 模糊快照（读写均不阻塞） | P7 + B+树 | M4 用"冻结写者"的一致快照；无锁一致快照要 COW/MVCC 结构 |
| 结构保留式快照盘上格式（如 B+树落页 + mmap 加载） | 未来可选 | M4 走通用记录流，可移植、简单 |
| 介质级冗余（RAID / 多副本 / 巡检）抵御静默位翻转 | 下层 / 未来 | M4 靠校验和检测 + WAL 重建兜底；不静默上当即可 |

---

## 2. 现状盘点（M1~M3 给了什么）

- **`RawDevice`**：O_DIRECT 裸设备 I/O——`Open` / `SizeBytes` / 任意 4K 对齐 `ReadAt`/`WriteAt` / `Sync()`（`fdatasync`）/ `AllocAligned`/`FreeAligned`。快照直接复用。
- **超级块**：三设备统一 8K 双份（主 @0 + 备 @4K），数据区从 `kDataRegionOffset`(8K) 起；快照设备 `device_type=Snapshot`、`block_count=0`，M1 已写/校验。数据设备超级块持 `block_count`（数据区块数）+ `engine_uuid`。
- **`MetaIndex`**（concept + `HashMetaIndex`）：`Insert`/`Lookup`/`Delete`/`Size`/`Contains`/`ForEach`；`ForEach(MetaIndexVisitor)` 当前返回 `void`；`WriteSnapshot(path)`/`LoadSnapshot(path)` 是返回 `kEngineNotImplemented` 的占位桩。`ValueMeta` = `block(8) + timestamp(8) + crc(4) + state(1)`（含 reserved 共 24 字节）。
- **`Wal`**：128 字节帧（`kWalKeyMax=84`、`kWalFrameSize=128`、双 CRC、`seq`）；公开 `Flush()`（攒批档刷净，同步档空操作）；私有 `seq_next_`（下一个 `seq`，从 1）。
- **`DeviceContext`**：`{io, wal, pool, block_allocator, meta_index, super_block}`——**无快照设备句柄**（M4 要加）。
- **`Options`**：`snapshot_threshold_bytes`（默认 512 MiB）、`snapshot_interval_sec`（默认 600s）已占位。
- **`util::AlignUp(x, a)`**：M3 已加。

---

## 3. 关键决策（已锁定）

| 编号 | 决策 | 结论 |
|---|---|---|
| **P5M4-D1** | 快照数据源 | **遍历内存索引（`ForEach`）写出**，不复制 WAL。语义上 ≡ "压缩后的 WAL"，机制上取自现成的内存索引；快照阶段**不读 WAL 设备** |
| **P5M4-D2** | 记录格式 | **128 字节定长 `SnapshotRecord`**（`key` + `ValueMeta` 各字段），只存活键、无墓碑；**不复用 `WalFrame`**；每条**不带** magic/version/seq/CRC；`key` 区容量 96 ≥ `kWalKeyMax`(84) |
| **P5M4-D3** | 完整性 | **整份一条 `data_crc`**（覆盖 `entry_count × 128` 字节）+ **槽头一条 `header_crc`**；二者分工：data 管内容、header 管槽头自身 |
| **P5M4-D4** | 设备布局 | 8K 双份超级块（M1）之后**对半切 A/B 两槽**；每槽 `[槽头 1×4K][记录数据区]`；槽头放槽首；A/B 起点 + 槽大小 Open 时算、常驻 |
| **P5M4-D5** | 槽头 | **恰好 4096 字节**（对标 `SuperBlock`）：`magic/version/generation/covered_seq/entry_count/data_len/data_crc/created_at/engine_uuid/header_crc32c` |
| **P5M4-D6** | 代际号 | `新代际 = max(槽头合法各槽 generation) + 1`（按**槽头合法**取，严格递增不撞号）；首份=1；**只存槽头**，重启读两槽头续号 |
| **P5M4-D7** | 写时选槽 + 写序 | 写**非恢复槽**（"最新可恢复槽"的另一个），保好覆坏；写序 = **数据 → 槽头 → 一次 `fdatasync`**；原子切换靠**代际 + `data_crc`**，无盘上"当前槽指针" |
| **P5M4-D8** | 读时选槽 | 读两槽头 → 校验 → **代际最大且双校验通过**者选中；坏则回退另一槽；都不行则 `covered_seq=0` 纯 WAL 重放（选槽逻辑本里程碑定，灌索引集成在 M6） |
| **P5M4-D9** | 落盘机制 | 每次快照**临时 `AllocAligned`** 一块 `snapshot_buffer_size`（默认 1 MiB、Options 可改、**不进池**、4K 对齐），攒满整块写、末尾补零到 4K、**全程一次 `fdatasync`** |
| **P5M4-D10** | `covered_seq` | M4 = `wal.last_seq()`（单线程下"已分配 = 已提交"）；**绝不报大**；`DoSnapshot` 顺序 **先 `Flush()` → 取 `covered_seq` → 写快照**；P7 须换"已提交水位" |
| **P5M4-D11** | 模块 + 接口 | 新建 `snapshot/`（平级 `wal`/`io`）；`Snapshot` 类持设备句柄，**回调解耦**（不认识索引后端）；`MetaIndex` concept **收窄**：删 `WriteSnapshot`/`LoadSnapshot`，`ForEach` 改返回 `int32_t` 可中止 |
| **P5M4-D12** | 触发 | 手动 `Engine::Snapshot()`（**同步、返回结果**）+ 大小阈值（**自动、fire-and-forget**：不连累 Put、出错记日志）；汇总入口 `RequestSnapshot()`；失败退避（`last_trigger_seq`）；**定时 → P7** |
| **P5M4-D13** | 容量约束 | **部署 Open 时校验、运行期不查**；快照设备 `slot_size ≥ 槽头 + ⌈block_count×128⌉₄ₖ`，WAL 设备 `≥ 阈值×2`；不足返回 `kDeviceTooSmall`。（P5M5 细化：WAL 侧落地为 `ring_size ≥ max(阈值×2, wal_buffer_size + 4K)`，见 §11 注） |
| **P5M4-D14** | 一致性模型 | M4 = **冻结写者的一致快照**（单线程下"冻结"为语义占位、不上真锁）；模糊/无锁一致快照 → P7 + B+树（结构相关、下沉后端） |
| **P5M4-D15** | 改动面 | 新建 `snapshot/`（3 文件）+ `test/snapshot/` + `test/common/test_env.h`；改 9 处现有文件（含 `util/crc32` 流式 CRC、`util/util.h` 缓冲规整）+ 3 个测试文件收敛 `GetEnv`；新增 2 个错误码（实装对账后的最终口径，见 §1.2/§12） |

---

## 4. 快照盘上格式

### 4.1 `SnapshotRecord`（128 字节定长）

一条记录 = 内存索引里**一个活键的索引项**（`key` + `ValueMeta`）。快照里只有活键（`Delete` 已从索引移除键），**无墓碑**。

```cpp
inline constexpr std::size_t   kSnapshotRecordSize    = 128;
inline constexpr std::size_t   kSnapshotKeyMax        = 96;   // ≥ kWalKeyMax(84)
static_assert(kSnapshotKeyMax >= kWalKeyMax, "快照 key 区必须能装下任何合法 key");

struct SnapshotRecord {
    std::uint16_t key_len;      // @0    实际 key 字节数（≤ kWalKeyMax）
    std::uint8_t  state;        // @2    ValueState（恒 Active；留位以备软删除入快照）
    std::uint8_t  reserved0;    // @3
    std::uint32_t value_crc;    // @4    value 的 CRC32C（来自 ValueMeta.crc）
    std::uint64_t block;        // @8    BlockId.raw
    std::uint64_t timestamp;    // @16   ValueMeta.timestamp
    std::uint8_t  key[96];      // @24   key，尾部补零
    std::uint8_t  reserved1[8]; // @120  预留扩展
};
static_assert(sizeof(SnapshotRecord) == kSnapshotRecordSize);
static_assert(std::is_standard_layout_v<SnapshotRecord>);
static_assert(std::is_trivially_copyable_v<SnapshotRecord>);
```

要点：
- **128 整除 4096 → 32 条/4K 块、永不跨页**，与 WAL 帧同一套对齐心智；补零浪费对"小、单版本、元数据量"的快照可忽略（换同尺寸的心智负担降低）。
- **不复用 `WalFrame`**：WAL 帧带 `magic/version/seq/entry_type/frame_crc32c`，快照记录都不需要（它是状态项不是日志操作）；省掉这些，128 里才腾出 96 字节的 key 区。复用的只是"128 字节定长"这个**尺寸约定**。
- **每条不带 CRC**：快照一次性全量、整体校验、坏了整槽回退，不需逐条 CRC；校验放"整份一条 `data_crc`"（§4.2）。
- **`key` 区 96 ≥ 84**：`Put` 入口已把 key 卡在 `kWalKeyMax`(84)，所以快照 key 一定 ≤ 84；96 是留头的余量。**有效 key 上限仍由 `Put`/`kWalKeyMax` 决定**——长于 84 的 key 根本进不来，那 12 字节是死余量（仅将来放宽 `kWalKeyMax` 时才用上）。

### 4.2 `SnapshotSlotHeader`（4096 字节，对标 `SuperBlock`）

```cpp
inline constexpr std::uint32_t kSnapshotSlotMagic   = 0x50414E53u; // "SNAP"
inline constexpr std::uint32_t kSnapshotSlotVersion = 1;
inline constexpr std::size_t   kSnapshotSlotHeaderSize = 4096;

struct SnapshotSlotHeader {
    std::uint32_t magic;          // @0     kSnapshotSlotMagic（认这是快照槽头，挡垃圾）
    std::uint32_t version;        // @4     槽头格式版本
    std::uint64_t generation;     // @8     代际号（单调递增；比大小定最新）
    std::uint64_t covered_seq;    // @16    覆盖到的最大 WAL seq（M5 回收 / M6 重放起点）
    std::uint64_t entry_count;    // @24    记录条数
    std::uint64_t data_len;       // @32    记录区字节长度（= entry_count × 128，交叉校验）
    std::uint32_t data_crc;       // @40    记录流 CRC32C（覆盖 entry_count × 128 字节）
    std::uint32_t reserved0;      // @44    对齐
    std::uint64_t created_at;     // @48    生成时间戳（util::GetWallTimeNs，诊断用）
    std::uint8_t  engine_uuid[16];// @56    引擎 UUID（与超级块比对，挡前朝残留槽）
    std::uint8_t  reserved[4020]; // @72    预留扩展，填零
    std::uint32_t header_crc32c;  // @4092  槽头自身 CRC32C，覆盖 [0, 4092)
};
static_assert(sizeof(SnapshotSlotHeader) == kSnapshotSlotHeaderSize);
static_assert(std::is_standard_layout_v<SnapshotSlotHeader>);
static_assert(std::is_trivially_copyable_v<SnapshotSlotHeader>);
```

字段要点：
- **`generation` 不复用 `covered_seq`**：两次快照间若无任何写，`covered_seq` 会相同，光看它分不出新旧；`generation` 是纯计数器，每次必 +1，永远能区分。
- **两 CRC 分工**：`header_crc32c` 管"槽头自己完整"（撕裂检测），`data_crc` 管"记录区内容正确"，缺一不可。
- **`engine_uuid`** 防"同一物理设备重建后旧槽残留被误用"——加载时比对 `slot.engine_uuid == 超级块.engine_uuid`，不符即拒（§7.3）。
- **`data_len`** 是 `entry_count × 128` 的交叉校验（对不上 → 槽头可疑）。

### 4.3 版本兼容纪律（现在立、迁移代码留未来）

固定六条，零成本、字段设计已具备：① `magic` 永不变、`version` 语义变更即 +1；② 加字段**只往 `reserved` 追加**；③ 退役字段**留死在原位不复用**；④ 结构体**永远 4096 / 128**；⑤ 读取**先验 magic 再看 version**；⑥ CRC 覆盖范围稳定。
真正的"新版引擎读旧版槽头"迁移代码留未来。**备注**：WAL 一旦经 M5 回收过 `covered_seq`，旧格式快照**不能直接丢弃重建**（会丢被回收掉的那段历史）——升级换格式时必须能读旧槽头或先离线迁移。

---

## 5. 快照设备布局与双缓冲两槽

```
快照设备字节布局：
0        4K        8K                                          设备尾
┌────────┬────────┬───────────────────────┬───────────────────────┐
│超级块  │超级块  │        A 槽           │        B 槽           │
│ 主份   │ 备份   │ [槽头4K][记录数据区]  │ [槽头4K][记录数据区]  │
└────────┴────────┴───────────────────────┴───────────────────────┘
@0       @4K      @kDataRegionOffset(8K)  @8K+slot_size
└──── 超级块 8K ──┘└──────── 快照区（对半切两槽）───────────┘
```

**Open 时算 A/B 布局，常驻内存**（挂 `Snapshot` 模块）：
```
可用区   = SizeBytes − kDataRegionOffset
slot_size = 向下取整4K( 可用区 / 2 )          // 必须向下取整
slot_a_off = kDataRegionOffset                // = 8K，天然 4K 对齐
slot_b_off = kDataRegionOffset + slot_size    // 两加数都 4K 对齐 → 也对齐
```
- **"向下取整 4K"身兼两职**：① 保证两槽都装得下（向上取整会让 B 槽冲出设备尾）；② `slot_size` 是 4K 倍数 → B 槽起点也 4K 对齐。两槽全程读写偏移 4K 对齐，满足 O_DIRECT。
- 每槽内部：**槽头放槽首**那一个 4K 块（加载定位最简），**数据区从 `槽起点 + 4K` 起**，`SnapshotRecord` 紧凑拼接、末尾补零到 4K；数据区容量 = `slot_size − 4K`。
- 之所以放槽首而非槽尾："数据 `data_crc` 重验 + A/B 回退"已能识别半截写，槽尾"末尾提交标记"那点好处用不上，槽首更简单。
- 容量校验（§11）**复用同一 `slot_size` 公式**，保证"校验过 = 实际装得下"。

---

## 6. 写流程：落盘机制 + 崩溃安全

### 6.1 `DoSnapshot`：一份快照从生到死

```cpp
int32_t Engine::DoSnapshot(DeviceContext& dc) {
    // 1. 先刷 WAL（默认级别 3 下是空操作；攒批档 2/4 才真刷）
    if (int32_t rc = dc.wal.Flush(); rc != err::kSuccess) return rc;
    // 2. 取 covered_seq（紧贴遍历；单线程独占，三步天然一致）
    const std::uint64_t covered_seq = dc.wal.last_seq();
    // 3~9. 交给 Snapshot 模块写一份（回调驱动遍历，见 §9）
    return dc.snapshot.Write(covered_seq,
        [&](const MetaIndexVisitor& v) { return dc.meta_index.ForEach(v); });
}
```

`Snapshot::Write` 内部（写**非活跃槽**）：
1. 申请 `snapshot_buffer_size` 对齐缓冲（临时，用完即弃）；
2. 经 `scan(enc)` 驱动 `ForEach`：每条 `SnapshotRecord` 编码进缓冲、累积 `data_crc` + `entry_count`；缓冲攒满整块 `WriteAt` 到目标槽数据区（`槽起点 + 4K` 起）；
3. 遍历完，末尾不足一块的**补零到 4K** 写出（补的零**不计入 `data_crc`**）；
4. 组装槽头（含 `generation = next_gen`、`covered_seq`、`entry_count`、`data_len`、`data_crc`、`engine_uuid`，最后算 `header_crc32c`），`WriteAt` 到目标槽起点那 4K；
5. **一次 `fdatasync`**；
6. 成功 → 更新内存缓存（§7.4）；失败 → 缓存不动、返回错误。

### 6.2 落盘机制（P5M4-D9）

- **临时缓冲、不进池**：快照低频（几分钟一次），`AllocAligned(snapshot_buffer_size)` 一次、`FreeAligned` 即弃；缓冲大小 ⊥ 索引大小，1 MiB 足以让 `WriteAt` 系统调用占比可忽略；**不借 `BufferPool`**（那是 value I/O 热路径的池，借用会串扰且尺寸不一定匹配）。
- **forward-only**：每页只写一次，绝不回头重写（与 WAL 同步档"同块重写 32 次"相反）。
- **全程一次 `fdatasync`**：快照是 all-or-nothing，中途无"已返回成功"的承诺，不需逐块持久；最后一次刷把数据 + 槽头一起压实。**不需要"刷两次"的屏障**——崩溃安全靠加载时重验 `data_crc`，不靠设备落盘写序（§6.4）。

### 6.3 写序：数据 → 槽头 → 刷一次（P5M4-D7）

**槽头物理在槽首，但程序里最后写**——因为它要装 `data_crc`，必须等数据全写完才算得出来（数据依赖逼出来的，不是偏好）。一次刷盘覆盖数据那几笔 + 槽头那一笔。

### 6.4 原子切换 + 崩溃点穷举

**没有盘上"当前槽指针"**：哪个槽最新，靠加载时**比 `generation`**（取最大且双校验通过者，§7.2）。所以**写新槽（更高代际）这个动作本身就是切换**，无单独翻指针步骤。原子性来自两层：① `header_crc` 让槽头"全有或全无"（撕裂的槽头校验不过 = 当不存在）；② `data_crc` 把槽头与数据绑死。

两根支柱使任一崩溃点安全：**① 写的是非活跃槽，旧（活跃）槽字节物理上从不被碰**（A/B 物理分开）；**② 新槽只有"槽头完整 + 代际更大 + `data_crc` 对"三道全中才被采纳**。逐点（设 A=代际5 在用、写 B=代际6）：

| 崩溃点 | 盘上状态 | 加载结果 |
|---|---|---|
| 写数据途中（槽头未写） | B 槽头还是旧代际/无效，数据被涂花 | B 代际小/无效 → **用 A**；旧快照完好 |
| 写完槽头、刷盘前 | 没刷 = 啥都不保证：新槽头没落地→用 A；落地但数据没全→`data_crc` 不过→拒 B 退回 A；全凑巧落地→用 B（等于成了） | 要么用完好 A、要么用完整 B，**绝不用坏的** |
| 刷盘后 | B 完整（代际6 + `data_crc` 对） | **用 B**，切换完成 |

凡"用 A"必意味新快照没落地 → WAL 没被回收过 A 的 `covered_seq`（回收与落地锁步，§8.3）→ **A + WAL 尾恢复到最新**，不丢数据。

> 原型姿态：崩溃 corner case 不抠到完备，覆盖上述常规崩溃可恢复即可。静默位翻转（无崩溃、AB 同坏）超出本模型——cabe 靠**校验和检测**（CRC 遍布，不静默上当）+ **快照可由 WAL 重建**兜底；介质级冗余（RAID/巡检）划在下层/未来。

---

## 7. 双缓冲：代际号、写时选槽、读时选槽

### 7.1 代际号（P5M4-D6）

`新代际号 = max(槽头合法的各槽 generation) + 1`。**按"槽头合法"（`header_crc` 过）取最大，而非"数据合法"**——这样即使某槽数据写坏，它的代际号也被消耗、永不撞号，保证全设备生命里代际严格递增（"谁代际大谁最新"这条铁律永真）。首份快照代际 = 1（0 作哨兵）。**只存槽头、不另设计数器**；重启（Open）读两槽头、`next_gen = max(合法代际) + 1`、`active_slot = 最新可恢复槽`，常驻内存。`u64` 永不溢出。

### 7.2 写时选槽 + 读时选槽 + 回退

- **写时（P5M4-D7）**：目标 = **"最新可恢复槽"的另一个**（"最新可恢复槽" = 代际最大且**槽头 + 数据都合法**的槽）；无可恢复槽 → 写 A 槽。边角"高代际槽数据坏、低代际槽完好" → **覆盖坏的、保住好的**（不是审美：覆盖好的会在"下次写又失败"时丢数据——好槽 + 未回收的 WAL 是真实恢复路径）。
- **读时选槽 + 回退（P5M4-D8，选槽逻辑本里程碑定，灌索引集成在 M6）**：
  ```
  读 A、B 两槽头 → 校验（magic / version / engine_uuid==超级块 / header_crc）
    → 「槽头合法」者按 generation 从大到小排
    → 逐个验数据：读 entry_count×128 字节、算 CRC 比 data_crc
        合法 → 选中，covered_seq = 槽头.covered_seq，结束
        不合法 → 回退下一候选
    → 都不行 → covered_seq = 0，恢复改为从 seq=1 纯 WAL 全量重放
  ```
  "槽头合法"用于排候选，"数据合法"用于最终选中；空槽 `magic=0` 自然排除。**实现建议（落 M6）**：一遍扫——边解析记录灌索引边累积 CRC，读完比对；不过则清空索引、回退试下一候选（只读一遍）。

### 7.3 加载侧无需结构特化

加载是单线程、无并发，只用后端的 `Insert`；选槽 + 解析 + 灌索引由 `Snapshot` 模块（结构无关）驱动，后端不必为加载做任何结构特化——这反证盘上格式属于共享模块、不属于后端。

### 7.4 内存缓存的更新时机（崩溃语义）

`Snapshot` 在内存缓存 `active_slot` / `next_gen` / `last_covered_seq`——它们是**盘上槽头的缓存,供写时/运行期控制**（免去重复读盘；`last_covered_seq` 与触发相关、近乎每写必用）。**只在 `fdatasync` 成功后才更新，失败不动**（触发退避用的 `last_trigger_seq` 例外、每次尝试都推进，§10）。

- **为什么必须等刷盘成功**：若刷盘前就更新 `active_slot`，而刷盘**无崩溃地失败**了 → 内存说"新槽是当前"、盘上却没落地 → 下一次快照会去覆盖**那份好的旧槽** → 丢数据。
- **崩在"刷盘成功、内存未更新"之间无害**：内存本就随崩溃丢光，下次 Open 从两槽头重建。**盘上代际号是唯一真相，恢复从不信内存。**

---

## 8. 与 WAL / 恢复衔接：`covered_seq`

### 8.1 精确语义（P5M4-D10）

`covered_seq` 是一句承诺：**"凡 seq ≤ covered_seq 的写，都已被这份快照收进去。"** 恢复 = `加载快照(≤covered_seq) + 重放 WAL(>covered_seq)`，它就是那一刀。关键不变量：**绝不报大**——报大了那条写既不在快照、又不被重放（在刀左边），就丢了；宁可报小（会被幂等重放一遍，无害）。

### 8.2 M4 取值 + 取值时机

- **M4 = `wal.last_seq()`**（= `seq_next_ − 1`，空时 0），单线程下"已分配 = 已提交"（每个 Put 整条做完才轮到下一个），用它不会报大。
- **顺序：先 `Flush()` → 取 `covered_seq` → 遍历写**（§6.1）。先刷 WAL 保证盖 `covered_seq=S` 那刻 WAL 真落到 S，给 M5/M6 一条干净不变量（任何已落地快照的 `covered_seq` 都指向真实在盘的 WAL 位置），消掉"覆盖了却没落盘"的边角；默认级别 3 下 `Flush()` 是空操作。
- **P7**：并发下"已分配序号"与"已提交到索引的水位"会分叉（`WriteWal` 分配 seq 与 `Insert` 之间有窗口）；`covered_seq` 须改用**已提交水位**，否则丢数据。M4 靠单线程独占白捡这份一致性。

### 8.3 回收与恢复的锁步铁律（M4 只记录、M5/M6 消费）

> **WAL 回收点 ≡ 最新已落地快照的 `covered_seq`，绝不越过它回收。先落地、后回收。**

它保证任一崩溃点都有一对 `(快照 + WAL尾)` 拼出最新（§6.4 末）。**M4 本身不回收**——WAL 线性追加、保留从 seq 1 的全部帧，回收点恒等于 0、铁律自动成立。M4 的全部职责就是**把 `covered_seq` 记准**（写进槽头），它是三方唯一契约：**M4 生产，M5 据它回收，M6 据它从 `covered_seq+1` 重放**。铁律到 M5 才真正"生效"。
（P5M5 实装注："据它回收"落地为**据同刻捕获的物理边界**——`DoSnapshot` 在 `Flush` 后与
`covered_seq` 成对捕获 WAL 当前窗口起点（covered_seq 的物理孪生），快照成功后回收到该偏移，
不做 seq→偏移换算。语义同一，见 P5M5 §6.2/§7.3。）

---

## 9. 快照模块 + 接口改造 + 编排

### 9.1 `snapshot/` 模块（与 `wal/`、`io/` 平级）

三设备三管理模块：数据设备 `IoBackend`、WAL 设备 `Wal`、快照设备 **`Snapshot`**。

```cpp
class Snapshot {
public:
    int32_t Open(const std::string& path, const Options* opts, const SuperBlock& sb);
    int32_t Write(std::uint64_t covered_seq, const ScanFn& scan);  // 写一份（§6）
    int32_t Close();
    // int32_t Load(const SinkFn& sink, std::uint64_t* covered_seq_out);  // 接口形状留位，实装在 M6
    std::uint64_t last_covered_seq() const noexcept;               // 触发用（§10）
private:
    RawDevice      dev_;                 // 自持快照设备（move-only，类比 Wal）
    std::uint64_t  slot_a_off_ = 0, slot_b_off_ = 0, slot_size_ = 0;
    std::uint64_t  next_gen_ = 1, last_covered_seq_ = 0;
    int            active_slot_ = -1;    // 最新可恢复槽（A/B/无）
    std::uint8_t   engine_uuid_[16]{};   // 盖槽头 + 加载比对
    const Options* opts_ = nullptr;      // 现读 snapshot_buffer_size
};
```

- **回调解耦**：`Snapshot` **不 `#include` 任何 MetaIndex 后端**，只认 `MetaIndexVisitor` 签名。
  ```cpp
  using ScanFn = std::function<int32_t(const MetaIndexVisitor&)>;  // 驱动后端遍历，返错即停
  using SinkFn = MetaIndexVisitor;                                 // 加载逐条灌索引（M6 用）
  ```
  `Engine` 把"具体哪个索引怎么遍历/插入"包成 lambda 注入；`Snapshot` 全权管缓冲/分槽/落盘/选槽。这正落实"格式/IO 上移共享模块、后端只管一致迭代"的缝。
- **`Snapshot` 自持 `RawDevice`、move-only**（移动时置空源设备句柄，避免双重释放），可随 `DeviceContext` 一起移动。
- **M4 实现 `Open`/`Write`/`Close`**；`Load` 接口形状留位、**实装在 M6**。

### 9.2 `MetaIndex` concept 收窄（P5M4-D11，推翻 P3 的 7 方法版）

- **从 concept 与 `HashMetaIndex` 移除 `WriteSnapshot`/`LoadSnapshot`**（它们被错放在后端：让一个内存结构去懂裸设备/盘上格式/`covered_seq` 是层次错位）。快照读写 I/O 全归 `snapshot` 模块。
- 后端只保留快照真正需要的两件，且都已存在：**`ForEach`（一致扫描源）+ `Insert`（加载落点）**。
- **`ForEach` 改为可中止**（P5M4-D11）：`MetaIndexVisitor` 与 `ForEach` 都返回 `int32_t`——回调返"成功"继续、返错误码就**立刻停并把错误传出**；`HashMetaIndex::ForEach` 遇非成功提前 `return`。趁现在 `ForEach` 几乎无调用方（仅一个契约测试）改它最便宜；整条链（编码回调 → `ForEach` → `ScanFn` → `Write`）统一 `int32_t` 传错误，去掉"捕获标志短路"。
  ```cpp
  using MetaIndexVisitor = std::function<int32_t(std::string_view, const ValueMeta&)>;
  // concept: { cidx.ForEach(visitor) } -> std::same_as<int32_t>;
  ```
- **`ForEach` 的一致扫描契约**：遍历所有活键，由后端保证视图一致——M4 单线程下就是直接遍历（"冻结写者"为语义占位、不上真锁）；P7 并发时由后端内部锁/拷贝/MVCC 保证不漏稳定键（结构相关、下沉后端，§14）。
- **`Clear()`**（M6 真恢复时"槽坏 → 清空索引回退"用）留 M6。

### 9.3 `DeviceContext` + `Engine` 编排

- `DeviceContext` 加 `Snapshot snapshot;` 成员（`wal` 旁），move-only 随之移动。
- `Engine::Open`（**仅 create 模式**，与 WAL 同分工；recover 模式的加载留 M6）：
  ```
  打开快照设备 → 算 A/B 布局常驻 → 跑容量校验（§11）→ 清空两槽头、next_gen=1、active=无
  ```
- `Engine::Snapshot()`（**手动**触发，公开）：逐设备 `DoSnapshot`，把成败 `return`（同步语义）。
- `Engine::Close`：逐设备关闭循环里加 `snapshot.Close()`，纳入"记首个错误、关完所有"。
- `Wal::last_seq()`：新增只读 `seq_next_ − 1`，供 `DoSnapshot` 取 `covered_seq`。

---

## 10. 触发

### 10.1 范围与契约（P5M4-D12）

| 触发源 | 里程碑 | 契约 |
|---|---|---|
| **手动 `Engine::Snapshot()`** | M4 | **语义 + 实现都同步**——做完返回成败（测试 + 手动刷用）；不走自动入口。recover 模式（M4 未打开快照设备）返回 `kEngineNotImplemented`（恢复编排在 M6），与自动路径的 `is_open` 守卫对称 |
| **大小阈值**（`snapshot_threshold_bytes`） | M4 | **语义 = fire-and-forget**（不返回结果、不让 Put 因快照失败而失败、出错记日志）；实现 = 同步内联（M4 限制） |
| **定时**（`snapshot_interval_sec`） | **P7** | 定时要后台线程；P7 后台快照的一部分 |

**汇总入口 `RequestSnapshot()`**：三个触发源（手动除外，手动直调）都汇到这一个内部入口。M4 里它**同步执行** `DoSnapshot`、出错记日志（fire-and-forget）；P7 只改它内部为"唤醒快照线程"。**抽象（入口）现在立好，机器（线程、并发合并）留 P7**，不造死代码。P7 加定时，就是给这个入口多接一个调用方。

### 10.2 大小阈值的度量与触发点

- **度量**：`增长量 = (wal.last_seq() − last_trigger_seq) × kWalFrameSize(128)`，即"距上次**尝试**以来多记了多少日志"。`≥ snapshot_threshold_bytes` 即触发。
- **触发点**：在 `Put`/`Delete` **成功收尾处**（`return Ok` 前，只成功路径，二者都查），抽成 `MaybeRequestSnapshot(dc)`，每次写按设备同步查（检查仅两整数相减一比较，极便宜）。
- **三层调用链**：
  ```
  MaybeRequestSnapshot(dc)  →  RequestSnapshot(dc)[自动 fire-and-forget]  →  DoSnapshot(dc)[真活，共用]
  Engine::Snapshot()  ─────────────────────────────────────────────────→  DoSnapshot(dc)[手动，返回结果]
  ```
- **`covered_seq` = 触发它那次写的 `seq`**（单线程下从检查到取序号无别的写插入）。
- 同步内联会让踩中阈值的那次写阻塞着把快照做完——**原型如此，性能留 P7 后台化**，不展开。

### 10.3 失败处理 + 退避（P5M4-D12）

- **铁定**：自动触发的快照失败**不连累 Put**（Put 已按级别成功落盘）、**只记日志**。
- **退避（避免设备坏时每写都白扫索引）**：加 `last_trigger_seq`，**每次尝试都推进**（成功/失败都推）；触发条件用它——失败也算"试过",下次要 WAL 再涨一个阈值才重试。`last_covered_seq`（恢复/回收用的"上次已落地覆盖点"）**仍只成功才推进**，两者职责分开、成功时相等。手动触发不受退避影响；持续失败 → WAL 涨,交 M5。（P5M5 注：M5 新增的**撞墙救援**路径也不受此退避约束——撞墙后写入失败、WAL 不再增长,闸门若拦它会卡死且无法自愈;见 P5M5 §8.2。）
  （实装注：推进点 = `Engine::DoSnapshot` **入口**的 `Snapshot::NoteTriggerAttempt`——记账放在"一次尝试的起点"，连"刷 WAL 失败"等还没到 `Write` 就失败的路径也被记账；否则攒批档（2/4）+ WAL 设备故障时每次写都会重试一遍注定失败的刷盘。）
- **重入**：M4 单线程下 `DoSnapshot` 不调 Put/Delete、不再请求快照，结构上重入不可能，**不加运行期防护**（仅在 `DoSnapshot` 写明"不得直接/间接触发 Put/Delete/再请求快照"的注释，可选加 Debug 断言）；并发合并留 P7 入口。

---

## 11. 部署期容量约束（P5M4-D13）

**部署 Open 时精确校验、运行期一律不查。** 上界是**精确硬顶**——每个活键恰占一个数据块，故 `活键数 ≤ block_count`，单份快照 ≤ `block_count × 128`。

```
单槽需求   = kSnapshotSlotHeaderSize(4K) + 向上取整4K( block_count × kSnapshotRecordSize )
快照设备 Open 校验：  slot_size（§5 同一公式） ≥ 单槽需求      // 否则 kDeviceTooSmall
快照设备部署建议：    SizeBytes ≥ 8K + 2 × 单槽需求 × 1.2~1.5  // 系数为格式演进余量，非条数不确定
WAL 设备   Open 校验：  SizeBytes ≥ snapshot_threshold_bytes × 2 // 否则 kDeviceTooSmall（实装推迟 M5，见下）
WAL 设备   部署建议：   SizeBytes ≥ snapshot_threshold_bytes × 2~3
```

> **实装注（M4 实际代码）**：**只实装快照设备的 Open 容量校验**；**WAL 设备的 Open 容量校验推迟到 M5**。原因——M4 测试用小 loop 设备（WAL 仅约 16 MiB），而默认阈值 512M × 2 = 1G 会把它们全部拒掉；且 M4 的 WAL 是线性追加、不回收、假定不写满，WAL 容量真正起约束是在 M5（环形回收）。`Wal::SizeBytes()` 已就位，M5 接上校验即可。
>
> **P5M5 已兑现 + 口径细化**：校验落 `Wal::Open`，公式为 `ring_size ≥ max(阈值×2, wal_buffer_size + 4K)`
> ——基准从裸 `SizeBytes` 细化为可用环容量 `ring_size`（对齐快照侧 `slot_size` 先例），并新增
> "缓冲+4K"下界（撞墙救援重试必成的数学前提）；既有测试夹具随之配小阈值。见 P5M5 §10。

- **快照设备**容量是精确硬顶，约为数据设备的 1/4096（一块 1 MiB 数据块对一条 128 字节记录、再 ×2 槽），放大很便宜。
- **WAL 设备**没有几何硬顶（同 key 反复改写 = 写放大无上限），其需求由**触发阈值**驱动（两次快照间 WAL 增长上限 = 阈值,再加快照写盘期间累积),**不能拿"≥快照"当基准**（小设备下会不够);真正兜底是 M5 环形回收。
- 校验在 **create 模式 Open** 跑（recover 模式随 M6）；不足 → `kDeviceTooSmall`，拒绝打开。容量公式绑定"定长 128 + 只存活键"两个格式不变量,将来若改格式须同步改公式。

---

## 12. 错误码 + 文件改动面 + CMake

- **新增 2 个错误码**（落新 snapshot 段，延续段链）：
  ```cpp
  inline constexpr int kSnapshotBase = -106000;            // = kWalRecoveryBase - kSegmentSize
  static_assert(kWalRecoveryBase - kSegmentSize == kSnapshotBase);
  kSnapshotWriteFailed = InSeg(kSnapshotBase, 0);          // 写/刷快照设备失败
  kDeviceTooSmall      = InSeg(kSnapshotBase, 1);          // Open 容量校验不过（快照/ WAL 设备）
  ```
  加载/损坏类错误码（`data_crc`/`header_crc` 不过等）随 `Load` 到 **M6**。
- **新建**：`snapshot/snapshot_format.h`、`snapshot/snapshot.{h,cpp}`、`test/snapshot/*`、`test/common/test_env.h`；`snapshot/CMakeLists.txt` + 顶层 `CMakeLists` 与 `test/CMakeLists` 挂上。
- **改 9 处现有文件**（实装对账后口径）：`engine/device_context.h`、`engine/engine.{h,cpp}`、`wal/wal.{h,cpp}`、`index/meta_index.h`、`index/hash/hash_meta_index.{h,cpp}`、`engine/options.h`、`common/error_code.h`、`util/crc32.{h,cpp}`（流式增量 CRC）、`util/util.h`（`RoundUpBufferSize`）；另 3 个既有测试文件（wal/engine/super_block）收敛 `GetEnv` 到共享测试头。

---

## 13. 测试设计

前提：M4 单线程、不崩溃、无恢复、**无 `Load`**——所以验证"写出来的快照对不对"**靠 `RawDevice` 把盘上字节读出来、用格式层解码/校验自由函数核对**，不靠 `Snapshot::Load`（M6）。

| 用例（实装名） | 验证点 |
|---|---|
| `RecordRoundTrip` | `SnapshotRecord` 编码→解码往返一致 |
| `SlotHeaderVerify` | 槽头 `header_crc` 校验：合法通过；坏 magic / version / 被覆盖字段被拒 |
| `StreamCrcMatchesOneShot` | 流式增量 CRC（分块累积）与一次性 `CRC32`（整段）结果一致 |
| `WriteSnapshotEndToEnd` | 手动 `Snapshot()` 写一份后读盘核对槽头各字段（代际/covered_seq/条数/data_len）+ 重算验 `data_crc` |
| `RecordsDecodeMatchPuts`（审查修复轮补） | **解码逐字段比对**：盘上记录解码后与 Put 的 key/补零/state/`value_crc`（与值重算比）/timestamp（时间窗，字段错位必跌出）/block 互异逐项核对——堵住"读回重算 CRC"循环验证查不出字段错位的盲区 |
| `AlternateSlotsAndGeneration` | 连写多份 → A/B 交替、`generation` 递增、写非活跃槽、covered_seq 递增 |
| `ThresholdAutoTriggers` | 写够量（阈值调小）→ 无需手动调用即自动落一份快照 |
| `ThresholdNotReachedNoSnapshot`（审查修复轮补） | **负半边**：不够量 → 不触发，两槽头均为无效态 |
| `OpenRejectsSmallDevice`（审查修复轮补） | 容量校验拒绝：**伪造 `block_count` 巨大的数据设备超级块**（`RawDevice` 仅支持块设备，不造小设备），打 `slot_size < need` 分支 → `kDeviceTooSmall` 且拒开后句柄已关 |
| `ForEachAbortsOnError` | 回调返错 → `ForEach` 提前停并把错误传出（落 `test/index/` 契约测试） |

- 槽头 `covered_seq == wal.last_seq()` 的核对并入 `WriteSnapshotEndToEnd`（3 个 Put → covered_seq=3）。
- **不测**：崩溃恢复（M6）、`Load`（M6）、回收（M5）、并发（P7）、value 的 FUA/异步持久性（崩溃才显形 → M6）；WAL 设备容量校验（随实装推迟 M5，见 §11 实装注）。
- **落点**：新建 `test/snapshot/`；loop 设备（数据 + WAL + 快照三路径）；**SetUp 把快照设备 8K 之后全区清零**（防 loop 设备跨用例残留误判，已实装）。

---

## 14. M4/M6 边界 + 未来演进（写入文档备查，非 M4 实现）

- **M4/M6 边界**：M4 = 只做**写流程**（Open/Write/Close）；**`Load` + 完整恢复编排（recover 模式 Open + 加载快照 + 重放 WAL + `RebuildFromActive`）= M6**。即便 M6 复用 §7.2 的选槽逻辑，灌索引与续写仍是 M6。
- **P7 注意事项（现在记、不实现）**：
  1. **`covered_seq` 口径**：并发下必须从"已分配 `last_seq`"换成"已提交水位"，否则那个 seq/提交窗口会丢数据（§8.2）。
  2. **一致扫描**：M4 的"冻结写者"是语义占位（单线程不上锁）；P7 由后端落实——哈希表用短锁/拷贝（rehash 会漏稳定键 = 丢数据，无法无锁），B+树用 COW/MVCC（这也是未来上 B+树的一大动因）。"冻结写者"若伸到设备 I/O 期间会拉长停写，P7 可改"短锁内拷贝、解锁后写"缩窗。
  3. **后台快照线程**：`RequestSnapshot` 内部改唤醒线程；定时器作为新调用方接入；并发合并（只许一份在飞）在入口做。
  4. **重入/合并防护**：随后台线程一起加。
- **静默腐坏**：介质级冗余（RAID / 多副本 / 纠删 / 巡检）划在下层 / 未来；cabe 现阶段靠校验和检测 + WAL 重建兜底。

---

## 15. 关联文档同步（本次设计对历史文档的推翻/修正）

> 本次设计推翻了 P3 对 `MetaIndex` concept 的描述，并细化了 README 的 P5M4 范围；以**当前设计为准**同步：

1. **`doc/P5/README.md`**：① P5M4 段——删除"`WriteSnapshot`/`LoadSnapshot` 参数改裸设备"的旧描述（实为**移除**二者），改为本稿摘要（snapshot 模块 / 双缓冲两槽 / covered_seq / 触发 / 部署容量校验 / `Load`→M6）；② 关键技术备忘"模糊快照"条订正为"M4 冻结写者一致快照，模糊/无锁留 P7"。
2. **`doc/P3/P3M2_meta_index_design.md`**（concept 权威设计稿）：加 P5M4 起的**收窄超越注记**——移除 `WriteSnapshot`/`LoadSnapshot`（快照 I/O 上移 snapshot 模块）、`ForEach` 改返回 `int32_t` 可中止（原 void）；原 7 方法描述保留作历史。
3. **`doc/P3/README.md`、`doc/P3/P3M4_convergence_design.md`**：在"`MetaIndex` 7 个方法"处加一行"（P5M4 起收窄为 5 方法 + `ForEach` 可中止，见 P5M4 设计稿）"。
4. **`doc/P5/P5M1_super_block_design.md`**：§7.1 设备尺寸校验处加交叉引用——"快照/ WAL 设备的部署期容量下界校验在 P5M4 引入（见 P5M4 §11）；此处 M1 只保证 ≥8K 能放下超级块"。
5. **里程碑重编号**（快照/环形拆分、恢复 M5→M6、收敛 M6→M7）已在前序"里程碑重拆"中同步至 README + P5M1/2/3 + 相关代码注释,本里程碑不重复。

---

## 16. 退出判定

1. `snapshot/`：`SnapshotRecord`(128B) + `SnapshotSlotHeader`(4096B) + 编解码/校验自由函数；`Snapshot` 类 `Open`/`Write`/`Close` 实装（`Load` 留 M6）。
2. 快照设备布局：8K 双份超级块 + 对半切 A/B 两槽；Open 算 A/B 布局常驻 + 容量校验（不足 `kDeviceTooSmall`）。
3. 写流程：遍历索引 → 1 MiB 临时缓冲流式写非活跃槽 → 数据→槽头→一次 `fdatasync`；`covered_seq` 记入槽头（先 `Flush()` 再取 `last_seq()`）。
4. 双缓冲：代际号取号/续号；写非恢复槽、保好覆坏；读时选槽 + 回退逻辑（灌索引留 M6）。
5. 触发：手动 `Engine::Snapshot()`（同步返结果）+ 大小阈值（自动 fire-and-forget、退避）汇到 `RequestSnapshot`；定时留 P7。
6. `MetaIndex` 收窄：移除 `WriteSnapshot`/`LoadSnapshot`，`ForEach` 改 `int32_t` 可中止；契约测试随之更新。
7. `Wal::last_seq()`、`DeviceContext.snapshot`、`Engine` 三处编排、`snapshot_buffer_size`、两个错误码到位。
8. `test_snapshot` 新增用例（§13）全绿；已有测试保持绿色。
9. 关联文档同步（§15）完成。
</content>
</invoke>
