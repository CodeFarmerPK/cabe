# Cabe P5-M2 设计：WAL 核心 + 帧格式 + 严格级别

> 本里程碑实现 WAL（预写日志）核心：新建 `wal/` 模块（单个 `Wal` 类），定义 128 字节固定帧格式，
> 实现 **WAL 级别 1（Strict，最严格）**，并把它**端到端接进 `Engine::Put/Delete`**——写入/删除时
> 先把 value 持久落盘、再把 WAL 帧同步落盘、再写内存索引,才返回。WAL 设备复用 M1 的 `RawDevice`，
> 与管理数据设备的 `IoBackend` 分离。本里程碑只做级别 1；级别 2/3/4 在 M3 叠加，崩溃恢复/重放在 M6。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P5 / M2 |
| 状态 | **设计稿** |
| 上游依赖 | P5M1（超级块 + `RawDevice` + create/recover + `DeviceContext`） |
| 下游依赖本里程碑 | P5M3（WAL 级别 2/3/4，复用 `Wal` 接口）、P5M6（恢复重放，复用帧格式 + 解码） |
| 退出判定 | 见 §12 |

---

## 1. 目标与范围

### 1.1 目标

1. 定义 128 字节固定 WAL 帧格式（`WalFrame`），含帧自身 CRC32C + value 的 CRC32C。
2. 新建 `wal/` 模块：`Wal` 类（按 concept 语义度设计接口，不漏裸设备细节），帧编码/解码、线性追加、同步落盘。
3. 实现 **WAL 级别 1（Strict）**：value FUA 持久 + WAL 同步落盘 + 写内存索引，三者完成才返回。
4. 把级别 1 端到端接进 `Engine::Put/Delete`；`DeviceContext` 增加 `Wal` 成员；`IoBackend` 增加 value 持久写（FUA）能力。
5. 错误码：`kWalBase` 段新增 WAL 运行期错误码。

### 1.2 交付范围

1. **`wal/wal_frame.h`**（新建）：`WalFrame` 结构 + `WalEntryType` 枚举 + 帧常量（`kWalMagic` / `kWalFrameVersion` / `kWalFrameSize` / `kWalKeyMax`）+ 单帧编码/解码/校验。
2. **`wal/wal.h` + `wal/wal.cpp`**（新建）：`Wal` 类 + `WalEntry`（调用方填的逻辑记录）。
3. **`engine/device_context.h`**（修改）：加 `Wal wal;` 成员。
4. **`engine/engine.cpp`**（修改）：`Open` 在 create 模式打开 `Wal`；`Put`/`Delete` 接入级别 1 WAL；旧块/墓碑块回收顺序调整。
5. **`io/sync/sync_io_backend.*` / `io/uring/io_uring_backend.*`**（修改）：增加 value 持久写（FUA = `pwrite/提交` + `fdatasync`）。
6. **`common/structs.h`**（修改）：删除 P5 早期的 WAL 帧**占位**常量（`kWalFrameHeaderSize` / `kWalMagic` / `kWalVersion` / `kWalOff*`），真常量迁入 `wal/wal_frame.h`；保留 `kDataRegionOffset`。
7. **`common/error_code.h`**（修改）：`kWalBase` 段加 `kWalKeyTooLong` / `kWalWriteFailed`。
8. **`wal/CMakeLists.txt`**（新建）+ 根 `CMakeLists.txt` / `engine/CMakeLists.txt`（修改）：新建 `cabe_wal` 库并接入。
9. **测试**：`test/wal/wal_test.cpp`（帧编解码往返 + 设备级端到端级别 1）。
10. **关联文档**：更新 `doc/P5/README.md`（M2/M6 范围重划）、`ROADMAP.md`（P5 章节）。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| WAL 级别 2/3/4 + 攒批缓冲 + 异步刷出 | P5M3 | M2 只做级别 1 这条最严格、最简单的同步路径 |
| 环形缓冲区（绕圈覆盖 + 头部回收 + 写满兜底） | P5M5 | 回收依赖快照；M2 只做线性追加，假定 WAL 不写满 |
| 崩溃恢复 / WAL 重放 / `RebuildFromActive` | P5M6 | M2 的 WAL 是"只写"；读回重建索引在 M6 |
| recover 模式下的 WAL（重放已有日志 + 续写） | P5M6 | M2 的 WAL 追加只做在 create 模式 |
| 真正的 TRIM（异步批量 BLKDISCARD） | P7 | M2 沿用 `TrimDeviceBlock` 空桩；TRIM 是纯优化，不影响正确性 |
| WAL 的 io_uring 异步化 / 零拷贝 | P7 | 需多线程并发让深队列/批量有意义；详见 §11 |
| 故障注入测试矩阵（断电、EINTR、熵失败） | 后续 | 需要故障注入基础设施 |

---

## 2. 现状盘点（M1 给了什么）

- **`util/raw_device.*`**：通用裸设备 I/O 工具——O_DIRECT 打开 + `SizeBytes` + 任意偏移 4K 对齐 `ReadAt/WriteAt` + `Sync()`（fdatasync）+ `AllocAligned/FreeAligned`。WAL 直接复用它。
- **`engine/super_block.*`**：三设备超级块的 create/recover + 校验；WAL 设备的超级块（`device_type=Wal`，头部双份 @0/@4K）已由 M1 写入/校验。
- **`engine/device_context.h`**：`DeviceContext { io; pool; block_allocator; meta_index; super_block; }`。M2 加 `wal`。
- **`common/structs.h`**：`kValueSize`(1 MiB)、`kDataRegionOffset`(8K)、`BlockId`、`ValueMeta`；以及 P5 早期的 WAL 帧**占位**常量（注释写明"届时迁入 wal 模块"）——M2 替换之。
- **`common/error_code.h`**：`kWalBase = -103000` 段**为空**，正是留给 WAL 运行期错误的。
- **`engine/options.h`**：`WalLevel { Strict=1, ValueSync=2, WalSync=3, Async=4 }`；`Options.wal_level` 默认 `WalSync`（占位，按 P5M1 约定 **M3 起真正生效**）。
- **`engine/engine.cpp`**：`Put`/`Delete` 当前**不写 WAL**；`TrimDeviceBlock` 是 P7 空桩。
- **`io/*`**：`IoBackend::Write(block_idx, buf)` 写 1 MiB 块，物理偏移 `kDataRegionOffset + block_idx*kValueSize`，已加 `block_idx` 越界守卫；当前写无 FUA。

---

## 3. 关键决策（已锁定）

| 编号 | 决策 | 结论 |
|---|---|---|
| **P5M2-D1** | M2 范围（M2/M6 边界） | **端到端级别 1**：`wal` 模块 + 帧 + 把级别 1 接进 `Engine`。级别 2/3/4 → M3；重放/恢复 → M6。（重划了 roadmap 原来"Engine 接线在 M5"的写法） |
| **P5M2-D2** | entry 类型 | `enum class WalEntryType { Put=1, Delete=2 }`；每帧恰一种类型,未来加类型不破坏帧格式；**Delete = 墓碑帧**（同一 128 字节格式,`block`/`value_crc` 填 0） |
| **P5M2-D3** | 帧格式 | 128 字节固定、native LE、standard-layout + trivially-copyable；含单调序号 `seq`(LSN)；**不加 generation**（cabe 模型无法做按版本恢复）；双 CRC（帧 CRC + value CRC） |
| **P5M2-D4** | I/O 粒度 | 持久写单位 = **4K 块**（O_DIRECT 约束）；128 字节帧是块内逻辑槽（32 帧/块,不跨块）；内存持有"当前 4K 块"缓冲,整块重写 |
| **P5M2-D5** | 级别内化 | 级别来自 Options,由 `Wal`/`IoBackend` 各自持有并自行决定落盘/返回时机；`Engine::Put/Delete` 是**级别无关的固定调用序列**,不含 `switch(级别)` |
| **P5M2-D6** | WAL 设备归属 | WAL 复用 `RawDevice`(**不走 `IoBackend`**);`Wal` 类持有 WAL 设备的 `RawDevice`,负责其生命周期(与管数据设备的 `IoBackend` 分离) |
| **P5M2-D7** | value 持久写 | 级别 1 下 value 必须 FUA 持久后才返回;**"持久"是 `Write` 的语义保证**,机制随后端实现(M2:sync 与 io_uring 后端都用 `fdatasync`),不向 `Engine` 暴露独立的 `fdatasync` 方法 |
| **P5M2-D8** | 写满兜底 | M2 **不判 WAL 满**(线性只增,假定不满);环形缓冲 + 头部回收 + 写满兜底 → M5。自然兜底:写过设备尾 `RawDevice` 返回 `kIoBase`(失败安全,不静默损坏) |
| **P5M2-D9** | create/recover | WAL 追加只做在 **create 模式**(从偏移 8K 开空日志,`seq` 从 1);recover 的重放 + 续写 → M6 |
| **P5M2-D10** | 块回收 | 沿用 `TrimDeviceBlock` 空桩(P7 做真 TRIM);**Put 的旧块在提交成功后才回收**(覆盖安全);Delete 的块在墓碑帧落盘后回收 |
| **P5M2-D11** | M2 的级别取值 | M2 只实装级别 1;`Options.wal_level` 的分级 **M3 起生效**,M2 阶段 WAL 统一按级别 1（最严格）运行 |

---

## 4. WAL 帧格式

128 字节固定帧。每帧基址是 128 的倍数（8 对齐），故帧内 8 对齐字段在内存中天然对齐,可作为 trivially-copyable 结构 memcpy 进出。

```cpp
// wal/wal_frame.h
#ifndef CABE_WAL_FRAME_H
#define CABE_WAL_FRAME_H

#include "common/structs.h"   // BlockId（key→block 映射用）

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cabe {

    inline constexpr std::uint32_t kWalMagic        = 0x45424143u; // "CABE"
    inline constexpr std::uint32_t kWalFrameVersion = 1;
    inline constexpr std::size_t   kWalFrameSize    = 128;         // 128 是 4096 的因数，32 帧填满一个 4K 块
    inline constexpr std::size_t   kWalKeyMax       = 84;          // key 最大字节数（超出 Put 报错）

    enum class WalEntryType : std::uint8_t {
        Put    = 1,
        Delete = 2,
        // 未来可扩展（如 checkpoint），每帧单一类型，不破坏格式
    };

    // 统一 128 字节帧。Delete（墓碑）复用本结构，block/value_crc 填 0。
    struct WalFrame {
        std::uint32_t magic;        // @0    kWalMagic
        std::uint8_t  version;      // @4    kWalFrameVersion
        std::uint8_t  flags;        // @5    预留标志位（M2 不用）
        std::uint8_t  entry_type;   // @6    WalEntryType
        std::uint8_t  reserved0;    // @7    对齐/预留
        std::uint64_t seq;          // @8    单调 LSN（排序 + 环形区消歧；M6 重放定边界）
        std::uint64_t block;        // @16   BlockId.raw（Delete 填 0）
        std::uint64_t timestamp;    // @24   util::GetWallTimeNs（还原 ValueMeta.timestamp / 未来 TTL）
        std::uint32_t value_crc;    // @32   value 的 CRC32C（Delete 填 0）
        std::uint16_t key_len;      // @36   实际 key 字节数（≤ kWalKeyMax）
        std::uint16_t reserved1;    // @38   对齐/预留
        std::uint8_t  key[84];      // @40   key，尾部补零
        std::uint32_t frame_crc32c; // @124  帧自身 CRC32C，覆盖 [0, 124)
    };

    static_assert(sizeof(WalFrame) == kWalFrameSize);
    static_assert(std::is_standard_layout_v<WalFrame>);
    static_assert(std::is_trivially_copyable_v<WalFrame>);

} // namespace cabe

#endif // CABE_WAL_FRAME_H
```

**字段说明**

- `magic` / `version`：识别帧 + 格式版本。读回时魔数或版本不符 → 视为非本格式帧（M6 重放据此定边界）。`version` 是**帧格式版本**，与"快照代际"无关。
- `entry_type`：`Put` / `Delete`。每帧单一类型，未来加新类型只是多一个取值，不改布局。
- `seq`：全局单调递增序号（LSN，从 1 起，0 作哨兵）。职责:① 重放定序;② 环形区绕圈时区分"本圈新帧"与"上圈残留旧帧"(同槽位旧帧 CRC 也合法,靠 `seq` 消歧)。u64 永不溢出(亿次/秒也要数百年)。**与时间戳不同**:时间戳(墙钟)会回退、会撞值、跨重启无意义,不能用作定序;`seq` 是活在 WAL 内容里的逻辑计数器,恢复时从最大 `seq` 续号,天然跨重启连续。
- `block`：`BlockId.raw`,记录 key→block 映射;Delete 填 0。
- `timestamp`：写入墙钟时间,用于还原 `ValueMeta.timestamp`、调试、未来 TTL;**排序权威用 `seq`,timestamp 仅作信息**。
- `value_crc`：value 数据的 CRC32C,读时校验;Delete 填 0。
- `key_len` + `key[84]`：key 实际长度 + 内容(尾部补零)。**超过 84 字节的 key 在 `Put` 时拒绝**(`kWalKeyTooLong`)。
- `frame_crc32c`：覆盖 [0,124),校验帧自身完整(检测半截写入 + M6 重放定边界)。

**双 CRC 职责**:`frame_crc32c` 管"帧自身完整"(写入是否完整、恢复边界);`value_crc` 管"value 数据正确"(读时校验)。二者分工不同,缺一不可。

（P5M6 兑现注：上述全部"M6 重放"预告已落地——`VerifyFrame` 成为恢复扫描的逐槽判据（与快照
`covered_seq` 合成"合法 ∧ seq > covered ⇔ 活帧"完备三分类）；"恢复时从最大 seq 续号"细化为
**锚帧 seq + 1**（撕裂碎片先从盘上抹除再续号，防撞号/断链两难，见 P5M6 §10.3）；timestamp
"还原 ValueMeta.timestamp"按"还原当年、不用现在"原则兑现。另：M2 的**同字节重写撕裂安全模式**
在 M6 两处复用——恢复时留窗块重灌后的续写、碎片抹除时的尾块重写。）

**字节序**:native LE。工程仅 Linux/x86_64,与 `SuperBlock` 一致用 native memcpy,不做跨端序列化。

---

## 5. 物理布局与 I/O 机制

### 5.1 WAL 设备布局

沿用 M1：WAL 设备头部 8K 为双份超级块（主 @0 + 备 @4K），**日志区从偏移 `kDataRegionOffset`(8K) 起**。

```
WAL 设备：
┌──────────┬──────────┬───────────────────────────────────────────┐
│超级块主份│超级块备份│  日志区：4K 块 0 | 4K 块 1 | … →（线性增长）  │
│  @0 4K   │ @4K 4K   │  每个 4K 块内 32 个 128 字节帧槽             │
└──────────┴──────────┴───────────────────────────────────────────┘
0          4K         8K
|<-- 头部 8K 超级块 -->|<-------------- 日志区 ----------------->|
```

帧紧凑排列、无空位：第 `i` 帧物理偏移 = `kDataRegionOffset + i*128`，连续填满块 0（32 帧）后进块 1。

### 5.2 I/O 粒度：4K 块为持久写单位

O_DIRECT 要求 4K 对齐写,而一帧只有 128 字节——**无法单独写 128 字节**。所以:

> **持久写单位是 4K 块;128 字节帧是块内逻辑槽。**

`Wal` 内存持有**一块 4K 对齐缓冲**(`AllocAligned` 在 `Open` 时申请一次,每个新块 `memset` 清零**复用同一块内存**,`Close` 时 `FreeAligned`)。

### 5.3 级别 1 的写入与落盘（同步）

```
WriteWal(entry)（级别 1）：
  1. 若当前块已满(slot==32)：cur_off += 4096；slot=0；memset(cur_buf, 0, 4096)   // 进入下一块
  2. 在 cur_buf + slot*128 处编码这一帧：填字段、seq = seq_next++、算 frame_crc32c
  3. slot++
  4. WriteAt(cur_off, cur_buf, 4096) 整块写 + dev.Sync()（fdatasync）   // 落盘后才返回
```

同一个 4K 块在填满前最多被重写 32 次（每多一帧重写一次）。4K 写很便宜（~几十 μs），且 WAL 在独立设备上、与 1 MiB value 写互不抢盘;相对每次 Put 的 1 MiB value 写,WAL 的写放大可忽略。

### 5.4 反复重写为何 torn-write 安全

- 块内**已写过的帧内容不再改变**,每次重写写进旧帧扇区的是**完全相同的字节**;即便这次 4K 写断电撕裂,旧帧扇区要么是原字节、要么没被写到(仍是原字节)——**旧帧不变**。
- 且旧帧在**上一次 `Sync` 时已经持久**。
- 真正在险的只有**这次新加、尚未 ack 的帧**;它 ack 前(`Sync` 完成前)崩溃,恢复时 CRC 不过被当"没写过",正确。
- **结论:已 ack 的帧永不丢(级别 1 的"不丢"成立);只有未 ack 的在途帧可能丢,而这正确。**

### 5.5 不判满（D8）

M2 日志区只增不减（回收在 M5），从 8K 线性往后填。M2 **不判满、不绕圈**,假定不会写满（mkloop 的 WAL 16 MiB ≈ 12 万帧,测试用不满）。若假定失效真写过设备尾,`RawDevice.WriteAt` 返回 `kIoBase`——失败安全,不静默损坏。

> **P5M5 起本节已兑现/超越**：日志区环形化（模环推进、到尾绕回），"假定不写满"解除——空间由
> head/tail 核算 + 写满兜底（强制快照救援 + `kWalFull`）接管；"写过设备尾返回 `kIoBase`"的
> 天然兜底自此**不再可达**（模环推进永不越界），背压职责移交空间核算。详见 `P5M5_wal_ring_design.md` §4/§8。

---

## 6. `Wal` 接口

按 D2/D5/D6：语义级方法、级别内化、不漏裸设备细节、自己持有 WAL 设备。

```cpp
// wal/wal.h
namespace cabe {

    // 调用方填的逻辑记录（seq 由 Wal 内部分配，frame_crc 由 Wal 内部计算）
    struct WalEntry {
        WalEntryType     type;        // Put / Delete
        std::string_view key;         // ≤ kWalKeyMax，否则 WriteWal 返回 kWalKeyTooLong
        BlockId          block;       // Put 有效；Delete 填 {}
        std::uint32_t    value_crc;   // Put = value 的 CRC32C；Delete 填 0
        std::uint64_t    timestamp;   // 来自 ValueMeta.timestamp（Engine 已算）
    };

    class Wal {
    public:
        Wal() noexcept = default;
        ~Wal();
        Wal(const Wal&) = delete;
        Wal& operator=(const Wal&) = delete;
        Wal(Wal&&) noexcept;                 // 移动（DeviceContext 经 std::move 入 vector）
        Wal& operator=(Wal&&) noexcept;      // 移动时置空源 buf_/dev_，避免双重释放

        // 打开 WAL 设备并持有级别。create 模式：起点 = kDataRegionOffset，当前块清零，seq=1。
        int32_t Open(const std::string& wal_path, WalLevel level);
        // 唯一写入入口：内部按级别决定落盘/返回时机（M2 只实现级别 1 = 同步落盘）。
        int32_t WriteWal(const WalEntry& e);
        int32_t Close();

    private:
        int32_t Append(const WalEntry& e);   // 编码一帧入当前块缓冲（分配 seq、算 frame_crc）
        int32_t Sync();                       // 当前块整块 WriteAt + fdatasync

        RawDevice     dev_;                   // 持有 WAL 设备（不走 IoBackend）
        std::byte*    cur_buf_   = nullptr;   // AllocAligned 的 4K 缓冲
        std::uint64_t cur_off_   = 0;         // 当前块设备偏移
        std::uint32_t slot_      = 0;         // 0..31
        std::uint64_t seq_next_  = 1;
        WalLevel      level_     = WalLevel::Strict;
    };

} // namespace cabe
```

> **P5M3 起本节 `Wal` 接口已改写**（下文代码为 M2 当时形态）：`Open` 改为 `Open(wal_path, const Options*)`、去掉 `level_`（改现读 `opts->wal_level`）、缓冲从单个 4K 块改为 `wal_buffer_size` 单块（同步/攒批两档共用）、新增公开 `Flush()`。详见 `P5M3_wal_levels_design.md` §6。
>
> **P5M5 起再次扩展**：新增环形成员（`ring_start_`/`ring_end_`/`head_off_`/`window_bytes_`）与
> 回收三口（`reclaim_boundary()` / `ReclaimUpTo()` / `head_off()`）；`Flush` 改"整块推进、半块留窗"
> （提前刷出不再留块尾空洞）。详见 `P5M5_wal_ring_design.md` §5~§7。

要点：
- **公共只有 `Open` / `WriteWal` / `Close` / `Flush`**；`Append`/`Sync` 私有。级别策略封装在 `WriteWal` 内部——M2 即"`Append` + `Sync`"（级别 1）；P5M3 按级别分支（同步档每帧落盘；攒批档只 `Append`、靠攒满/Close/切档收紧触发 `Flush()`，后台刷出推迟 P7）。
- **职责划分**：调用方给逻辑内容（type/key/block/value_crc/timestamp）；`Wal` 拥有 `seq`、帧 CRC、设备偏移、4K 块缓冲——`Engine` 看不到裸设备（满足 D2）。
- **移动语义**：`DeviceContext` 会被 `std::move` 进 `devices_`，故 `Wal` 必须可移动且移动时置空源 `dev_`/`cur_buf_`，避免双重 close / 双重 free（与 `RawDevice`/`IoBackend` 同款）。
- （备注）公共 `Flush()` 在 **P5M3** 新增（攒批档刷出 / 切档收紧 / Close 用）；M4 快照前"强制刷净 WAL 缓冲"复用它;M2 不需要(级别 1 每帧即落盘)。

---

## 7. Engine 集成

### 7.1 DeviceContext

```cpp
struct DeviceContext {
    IoBackendImpl   io;              // 管数据设备
    Wal             wal;             // 管 WAL 设备 ← M2 新增
    BufferPool      pool{0};
    BlockAllocatorImpl block_allocator;
    MetaIndexImpl   meta_index;
    SuperBlock      super_block{};
};
```

### 7.2 Open（仅 create 模式接 WAL）

```
Engine::Open(opts)：
  ... M1 已有：CreateDeviceGroup / RecoverDeviceGroup 校验三设备超级块 ...
  dc.io.Open(cfg.data_path)
  dc.io.BlockCount() 一致性兜底
  dc.pool = BufferPool(kDefaultPoolBlocks)
  dc.block_allocator.Init(0, dc.super_block.block_count)
  if (opts.create)                                  // ← M2：仅 create 模式开 WAL（D9）
      dc.wal.Open(cfg.wal_path, opts.wal_level)     //   create：从 8K 开空日志，seq=1
  // recover 模式：M2 不开 WAL 追加（重放/续写 → M6）
```

> M2 阶段 WAL 统一按级别 1 运行（D11）；`opts.wal_level` 被透传保存以备 M3，但分级 M3 才生效。

### 7.3 Put（级别 1，固定序列 + 级别无关）

```
Put(key, value)：
  1. key 空 → kMemEmptyKey
  2. key.size() > kWalKeyMax → kWalKeyTooLong          // 新增，最早检查、无副作用
  3. value.size() != kValueSize → kEngineInvalidValue
  4. block = allocator.Acquire()                       // 满 → kEngineNoSpace
  5. buf = pool.Allocate()（失败 → recycle(block)；kEnginePoolExhausted）
     memcpy(buf, value)；value_crc = CRC32(value)
  6. io.Write(block, buf)                               // 级别 1：pwrite + fdatasync（value 落盘）
     pool.Free(buf)
     失败 → recycle(block)，返回错误
  7. wal.WriteWal({Put, key, block, value_crc, now})    // 级别 1：整块落盘 + fdatasync
     失败 → recycle(block)，返回错误（不动索引、不动旧块）
  8. old = meta_index.Lookup(key)（记下旧块,若有）
     meta_index.Insert(key, {block, now, value_crc, Active})
  9. 若有旧块：recycle(old.block)；TrimDeviceBlock(old.block)   // 提交成功后才回收旧块
  10. 返回成功
```

**排序道理**：
- **value 在 WAL 前**（6 在 7 前）：级别 1 要保证"WAL 一旦记下 key→块,块里就真有对应 value"——否则恢复后索引指向未写的块。
- **内存在 WAL 后**（8 在 7 后）：预写日志原则（WAL = Write-Ahead Log）；且若 WAL 写失败,内存未动,返回的错误与状态一致。
- **旧块最后回收**（9）：新映射提交成功后才回收旧块。`Acquire`(第 4 步)拿的一定是空闲块,而旧块在第 9 步前仍被索引引用、不在空闲列表,所以新块 ≠ 旧块,不会原地覆盖旧 value——这正是"旧块最后回收"的安全前提。

### 7.4 Delete（级别 1）

```
Delete(key)：
  1. key 空 → kMemEmptyKey
  2. meta = meta_index.Lookup(key)                  // 不存在 → kIndexKeyNotFound（不写 WAL）
  3. wal.WriteWal({Delete, key, {}, 0, now})         // 墓碑帧，级别 1 落盘
     失败 → 返回错误（不动内存）
  4. meta_index.Delete(key)
  5. recycle(meta.block)；TrimDeviceBlock(meta.block)
  6. 返回成功
```

Delete 不写 value,只有 WAL 一处落盘,比 Put 简单。墓碑帧的 key 一定 ≤ 84（它先前 Put 成功过,已过长度检查）。

### 7.5 value 持久写（FUA）

级别 1 的 value 必须持久后才返回。**"持久"是 `IoBackend::Write` 的语义保证,机制由后端实现**（D7）：

- **sync 后端**：`pwrite` + `fdatasync`。
- **io_uring 后端**：提交写 + 等完成 + `fdatasync`（M2 也补上 `fdatasync`,保证级别 1 在两个后端都正确）。

M2 只做级别 1（value 始终 FUA）。级别 3/4 的"value 异步"是 M3,届时 `IoBackend` 按级别分支。**不向 `Engine` 暴露独立 `fdatasync` 方法**——`Engine` 只调 `io.Write`,持久与否由后端按级别保证。机制随后端可换的完整论述见 §11。

### 7.6 崩溃一致性（为什么这套是对的）

- **崩溃时内存整块消失**;恢复(M6)从 WAL 重放重建索引——**WAL 是唯一真相,内存是可重建的缓存**。
- 任意崩溃点:已 ack 的写/删,其 WAL 帧已持久,恢复后状态一致;未 ack 的,丢失即可(调用方未收到成功)。
- 数据盘上残留的旧 value 字节**不算不一致**:一个块是否"活"由(重放出的)索引决定,没有索引指向它 = 空闲垃圾,`RebuildFromActive`(M6)从活索引反推空闲块,自然把它算空闲。

---

## 8. 错误码

`common/error_code.h` 的 `kWalBase`（-103000）段新增：

```cpp
inline constexpr int kWalKeyTooLong  = InSeg(kWalBase, 0); // -103000  key 超过 kWalKeyMax（Put 拒绝）
inline constexpr int kWalWriteFailed = InSeg(kWalBase, 1); // -103001  WAL 设备写/落盘失败（区别于数据盘 IO 错）
```

- `kWalFull`（WAL 写满）**留待 M5**（环形缓冲）。（P5M5 已兑现：-103002，仅快照救援无效时对外可见。）
- WAL 设备 IO 错由 `Wal` 内部把 `RawDevice` 的 `kIoBase` 翻译成 `kWalWriteFailed`,便于运维区分"WAL 盘"与"数据盘"故障。
- 注:项目后续会对错误码做一次系统性补齐/重构,此处先满足 M2 所需。

---

## 9. 目录与 CMake

```
wal/
├── wal_frame.h        # 新建：WalFrame / WalEntryType / 帧常量 / 单帧编解码
├── wal.h              # 新建：Wal 类 + WalEntry
├── wal.cpp            # 新建
└── CMakeLists.txt     # 新建

test/wal/wal_test.cpp  # 新建
```

```cmake
# wal/CMakeLists.txt
add_library(cabe_wal STATIC wal.cpp)
target_link_libraries(cabe_wal PUBLIC cabe_util)   # 用 RawDevice + CRC32；含 cabe_common 传递
add_library(cabe::wal ALIAS cabe_wal)
```

- 根 `CMakeLists.txt`：加 `add_subdirectory(wal)`。
- `engine/CMakeLists.txt`：`cabe_engine` 的 `target_link_libraries` 加 `cabe_wal`。
- `test/CMakeLists.txt`：注册 `test_wal`（链接 `cabe::wal`；设备级用例靠 `CABE_TEST_*` 环境变量门控,缺则跳过）。
- `common/structs.h`：删除 WAL 占位常量（删前确认无引用）。

---

## 10. 测试设计

前提：重放/恢复在 M6,故 M2 只测"帧是否正确写出 + 写路径是否通",不测"崩溃恢复"。

| 用例 | 需要设备 | 验证点 |
|---|---|---|
| `FrameRoundTrip` | 否（CI 可跑） | `WalEntry` → 编码 `WalFrame` → 解码 → 字段一致 + `frame_crc32c` 校验通过 |
| `FrameVersionMagic` | 否 | 错误魔数/版本的帧被判非法 |
| `PutWritesFrame` | 是（3 盘 create） | Put 一个 key → `RawDevice` 读 WAL 设备 @8K 第 0 槽 → 帧字段(magic/key/block/value_crc/seq/frame_crc)正确 |
| `DeleteWritesTombstone` | 是 | Delete → 写出 `entry_type=Delete`、block/value_crc=0 的墓碑帧 |
| `KeyTooLong` | 是 | Put 一个 > 84 字节 key → 返回 `kWalKeyTooLong`,无副作用 |
| `BlockAdvance` | 是 | 连续 Put 33 次 → 第 33 帧落到第二个 4K 块开头(@8K+4096) |
| `ValueDurable` | 是 | 沿用 M1 思路:读数据盘校验 value 已写到 block 0(物理偏移 kDataRegionOffset) |

**不测**（推迟）：崩溃恢复/重放（M6）；故障注入（断电、EINTR、熵失败）。

---

## 11. 未来演进（写入文档备查，非 M2 实现）

WAL 的 I/O 机制分两层:**底层机制**(P5:`RawDevice` 同步) 与 **上层语义**(`Wal`=4K 帧 / `IoBackend`=1MiB 块)。语义接口固定,机制随后端/时代可换。

### 11.1 并行化（P7）

- 现在 cabe 单线程同步:同一时刻最多一个写在飞,无并发可批量,所以 WAL 用同步 `RawDevice` 即可。
- P7 多线程 + 深队列后,WAL 才值得 io_uring 异步化:**提交不阻塞 + 完成通知(cqe)告知落盘**,多写并发各自独立确认。
- 届时"快内存操作与慢 WAL IO 重叠"才可能:异步发 WAL → 同时干别的 → 等 WAL 完成 → 返回。注意:即便异步,**改内存仍应在 WAL 确认落盘之后**(失败一致性)。

### 11.2 持久机制随后端可换

`IoBackend` 三后端各用其时代最合适的持久手段,对上层是同一语义("级别 1/2 的 `Write` 返回即持久"):

| 后端 | value 持久机制 | 顺带 |
|---|---|---|
| sync（M2） | `pwrite` + `fdatasync` | 简单、正确、与超级块一致 |
| io_uring（P7） | 写 SQE 带 `RWF_DSYNC`(单笔 FUA) + 注册缓冲 | 并行 + 零拷贝 + 单 op 持久 |
| spdk（更远） | NVMe Write + FUA 位 + hugepage DMA | 用户态、零拷贝、单命令持久 |

- `fdatasync` / NVMe Flush = 整盘刷、阻塞屏障,适合"攒一批再一次刷";`RWF_DSYNC` / NVMe FUA = 每笔自带持久、完成通知确认,天然配合并发。
- **SPDK 不同点**(待接入 SPDK 时再确认):用户态 NVMe 驱动,无 fd / `pwrite` / `fdatasync`,直接发 NVMe 命令,落盘靠命令的 FUA 位或 Flush 命令。
- WAL 的 io_uring 优化亦同:走**共享的底层异步原语**(`RawDevice` 的异步兄弟),**不经 IoBackend 块 API**(语义不匹配);`Wal` 公共接口不变,只换内部机制。

### 11.3 其它推迟项

- **零拷贝**：io_uring 注册缓冲 / SPDK 的 DMA 缓冲;与持久机制正交,但 `RWF_DSYNC`/FUA 能与之融成一个 op。P7。
- **环形缓冲 + 头部回收 + 写满兜底**：依赖快照,P5M5。
- **崩溃恢复 / WAL 重放 / recover 续写**：P5M6。重放用 `seq` 定边界、消环形区绕圈歧义。（P5M6 注：已设计落定——两遍扫描 + 走读单判据"下一槽=下一 seq"，见 P5M6 稿。）
- **真正的 TRIM**：异步批量 `BLKDISCARD`,P7;M2 沿用空桩。
- **WAL 缓冲区大小运行时可调**：P5M3 已定缓冲大小 Open 时定死、运行期固定；运行时改大小进一步推迟到未来 Options 维护接口。

---

## 12. 退出判定

1. `wal/wal_frame.h` 帧格式落地：128 字节、`static_assert(sizeof==128)`、`FrameRoundTrip` 通过。
2. `Wal` 类实装：`Open(create)` / `WriteWal`(级别 1 同步落盘) / `Close`;线性追加 + 4K 块整块重写。
3. `Engine::Put/Delete` 接入级别 1：value FUA + WAL 同步 + 写内存,固定序列、级别无关;旧块/墓碑块按 D10 回收。
4. `IoBackend`(sync + io_uring)增加 value FUA 持久写。
5. `kWalBase` 段新增错误码;`structs.h` 占位常量替换。
6. `cabe_wal` 库接入;`test_wal` 全绿（设备级用例在 3 块 loop 设备上跑）。
7. 关联文档更新：`doc/P5/README.md`（M2/M6 范围重划）、`ROADMAP.md`（P5 章节）。
8. 已有测试（M1 超级块、engine、io、slots 等）保持绿色。
