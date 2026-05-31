# Cabe P0-M2 设计：`common/structs.h` Schema 定型

> 本里程碑把 P0-M1 起始终未改动的 `common/structs.h`，从早期草稿 schema 收敛到 ROADMAP
> 锁定的最终数据模型（D1–D5、D13、D14）；并连带适配唯一依赖该 schema 的编译单元
> `util/crc32.cpp`。**本文为详细设计，暂不生成代码**；其中的 C++ 片段均为设计示意。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P0 / M2 |
| 状态 | **✅ 已锁定（P0M7 收敛）** |
| 上游依赖 | M1（可构建骨架；`structs.h` 在 include 根下可被 `cabe_util` 引用） |
| 下游依赖本里程碑 | M3（错误码/日志）、M4（hash 用 `DataView`）、M5（schema 单测）、P1+（`Engine` 全程用本 schema） |
| 关联架构决策 | D1（value 1 MiB）、D2（设备无 header）、D3（元数据仅 RAM/WAL）、D4（命名分层）、D5（BlockId 8/56 编码）、D13（WAL 帧头）、D14（CRC32C） |
| 退出判定 | `structs.h` 编译通过、无旧名残留、全部 `static_assert` 通过；`cabe_util`（含适配后的 `crc32.cpp`）双工具链构建通过 |

---

## 1. 目标与范围

### 1.1 目标

把 `common/structs.h` 从草稿态（`CABE_VALUE_DATA_SIZE` / 裸 `BlockId` / `ChunkMeta` /
`DataState` / `span<char>`）改造为 ROADMAP 锁定的最终数据模型，并保证整个 `cabe_util`
库在新 schema 下双工具链可构建。

### 1.2 交付范围（本里程碑产出）

1. 重写 `common/structs.h`：`kValueSize` / `DeviceId` / `DataView`·`DataBuffer`(std::byte) /
   `BlockId`(8·56 编码 struct) / `ValueState` / `ValueMeta`(24 字节) / WAL 帧头占位常量。
2. 删除旧名：`CABE_VALUE_DATA_SIZE`、`ChunkMeta`、`DataState`、裸 `using BlockId`。
3. 编译期断言：`sizeof(BlockId)==8`、`sizeof(ValueMeta)==24`、对齐与可平凡复制断言。
4. **连带适配** `util/crc32.cpp`：`DataView` 元素由 `char` 变 `std::byte`（见 §7）。
5. 顺手清理 `util/util.h` 注释中的草稿残留名 `KeyMeta.createdAt`。

### 1.3 推迟范围（明确推迟，标注落点）

| 推迟项 | 落点 | 原因 |
|---|---|---|
| WAL 帧头的真实编解码（读写、双 CRC、4 KiB 对齐） | **P5** | M2 只放布局占位常量，WAL 模块在 P5 |
| WAL 持久化字节序（magic / 多字节字段的端序） | **P5** | M2 的 struct 是内存表示，端序属持久化层 |
| `entry_type` 取值枚举（`PutCommit` / `Delete` …） | **P5** | 帧语义在 P5 定义 |
| `ValueMeta` 的序列化/反序列化函数 | **P5（WAL）/ P3（snapshot）** | M2 只定型内存布局 |
| `BlockId` 的 `std::hash` 特化 | 需要时（P4.5 FreeList / 索引若用哈希容器） | 当前 FreeList 用 `vector`+排序，未用哈希 |
| `kValueSize` 校验逻辑（`value.size()!=kValueSize` 拒绝） | **P1（Put 路径）/ P2（API）** | M2 只定义常量，校验在调用路径 |

---

## 2. 现状盘点（读码结论）

当前 `common/structs.h`（全局命名空间）：

```cpp
inline constexpr size_t CABE_VALUE_DATA_SIZE = 1024 * 1024;
using BlockId    = uint64_t;            // 裸 alias，无 device/idx 编码
using DataView   = std::span<const char>;
using DataBuffer = std::span<char>;
enum class DataState : uint8_t { Active = 0, Deleted = 1 };
struct ChunkMeta { BlockId blockId; uint32_t crc; uint64_t timestamp; DataState state; };
```

**依赖该 schema 的代码（决定连带改动面）**：
- `util/crc32.h`：`namespace cabe::util { uint32_t CRC32(DataView); }` —— 仅用 `DataView`
  类型名，签名文字不随 `DataView` 底层类型改变而变。
- `util/crc32.cpp`：`SoftwareCRC32C` / `HardwareCRC32C_x86` 内部按 `char` 遍历 `DataView`
  并 `data.data()`（返回 `const char*`）—— **`std::byte` 化后必须适配**（§7）。
- 其余编译单元：`util/cpu_features.{h,cpp}` 不含 schema 依赖；无别处引用
  `CABE_VALUE_DATA_SIZE` / `ChunkMeta` / `DataState`（当前仅 2 个 `.cpp`）。

**结论**：M2 的代码改动面 = `structs.h`（重写）+ `crc32.cpp`（`std::byte` 适配）。
`crc32.h` 签名不变。

---

## 3. 待 owner 终审的决策（review 时优先裁决）

### 决策-1：为 schema 类型引入 `cabe` 命名空间

| 维度 | 内容 |
|---|---|
| ROADMAP 字面 | 仅给出类型/字段，未提命名空间；当前 `structs.h` 在**全局命名空间** |
| 本设计裁决 | **建议采纳**：把 `kValueSize` / `DeviceId` / `DataView` / `DataBuffer` / `BlockId` / `ValueState` / `ValueMeta` / WAL 常量统一纳入 `namespace cabe`（与 `cabe::util` 分层一致） |
| 理由 | `DataView` / `BlockId` 等是通用名，留在全局命名空间有符号冲突隐患；`util/` 已在 `cabe::util`，schema 进 `cabe::` 顶层形成清晰分层 |
| 影响 | `crc32.{h,cpp}` 在 `cabe::util` 内引用 `DataView`，名称查找会向上落到 `cabe::DataView`，无需改写引用；外部将以 `cabe::BlockId` 等限定访问（当前无外部调用方） |
| 回退代价 | 低：去掉 `namespace cabe { }` 包裹即可；但**建议作为全工程约定**（schema→`cabe::`，工具→`cabe::util`），与 M3 的 `error_code` / `logger` 命名空间策略一并锚定 |

### 决策-2：`ValueMeta` 字段顺序重排（达成 `sizeof==24`）

| 维度 | 内容 |
|---|---|
| ROADMAP 字面 | `struct ValueMeta { BlockId block; uint32_t crc; uint64_t timestamp; ValueState state; }` 且要求"24 字节" |
| 问题 | **这两条自相矛盾**：按字面成员顺序自然对齐，`sizeof` 是 **32**，不是 24（详算见 §6） |
| 本设计裁决 | **采纳重排**为 `{ block, timestamp, crc, state }`，自然对齐下 `sizeof==24`（无需 `#pragma pack`，保持对齐友好） |
| 理由 | 24 字节是 D 决策硬要求（且带 `static_assert(==24)`）；自然对齐下只有把 8 字节字段前置才能达成，重排是唯一不牺牲对齐的方案 |
| 回退代价 | 若终审坚持字面顺序，则要么接受 32 字节（违反"24 字节"），要么 `#pragma pack(1)`→21 字节（破坏对齐、且仍非 24）。故重排是唯一自洽解 |

> 这两处一旦终审反转，受影响章节为 §4 全貌、§5.6、§6、§8 决策表。

---

## 4. 目标 schema 全貌（最终 `structs.h` 形态示意）

```cpp
#ifndef CABE_STRUCTS_H
#define CABE_STRUCTS_H

#if !defined(__linux__)
#  error "Cabe currently only supports Linux (target: Fedora 43). See README.md."
#endif

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace cabe {

// ---- 固定 value 大小（D1）----
inline constexpr std::size_t kValueSize = 1024 * 1024;   // 1 MiB

// ---- 设备标识（D5：占 BlockId 高 8 位）----
using DeviceId = std::uint8_t;

// ---- 数据视图（D2/D4：设备上只有裸字节）----
using DataView   = std::span<const std::byte>;
using DataBuffer = std::span<std::byte>;

// ---- 物理寻址（D5：device_id:8 | block_idx:56）----
struct BlockId {
    std::uint64_t raw;

    static constexpr std::uint64_t kIdxBits = 56;
    static constexpr std::uint64_t kIdxMask = (std::uint64_t{1} << kIdxBits) - 1;

    static constexpr BlockId Make(DeviceId dev, std::uint64_t block_idx) noexcept {
        assert(block_idx <= kIdxMask && "block_idx exceeds 56 bits");  // Debug 防线；NDEBUG 下消除
        return BlockId{ (static_cast<std::uint64_t>(dev) << kIdxBits) | (block_idx & kIdxMask) };
    }
    constexpr DeviceId      dev()         const noexcept { return static_cast<DeviceId>(raw >> kIdxBits); }
    constexpr std::uint64_t block_idx()   const noexcept { return raw & kIdxMask; }
    constexpr std::uint64_t logical_byte_offset() const noexcept { return block_idx() * kValueSize; }  // 逻辑偏移=块号×块大小；物理偏移 P5 起由 IoBackend 加 kDataRegionOffset(8K)

    // C++20：defaulted <=> 自动合成 == 与全部关系运算（FreeList 排序 / 一致性比较用）
    constexpr auto operator<=>(const BlockId&) const noexcept = default;
};
static_assert(sizeof(BlockId) == 8);
static_assert(std::is_trivially_copyable_v<BlockId>);

// ---- value 状态（替换 DataState）----
enum class ValueState : std::uint8_t {
    Active  = 0,
    Deleted = 1,
};

// ---- value 元数据（D3：仅存在于 RAM / WAL）----
// 字段顺序为达成 sizeof==24 而重排（见 §3 决策-2、§6）；末尾 reserved[3] 把隐式 padding
// 显式化并清零，保证整体可确定地 memcpy 序列化（见 §5.6）。
struct ValueMeta {
    BlockId       block;            // 物理位置                       @0  (8)
    std::uint64_t timestamp;        // 写入时间，util::GetWallTimeNs   @8  (8)
    std::uint32_t crc;              // value 的 CRC32C（D14）          @16 (4)
    ValueState    state;            // Active / Deleted                @20 (1)
    std::uint8_t  reserved[3] = {}; // 显式占位 + 预留小扩展位          @21 (3)
};
static_assert(sizeof(ValueMeta)  == 24);
static_assert(alignof(ValueMeta) == 8);
static_assert(std::is_trivially_copyable_v<ValueMeta>);
static_assert(std::is_standard_layout_v<ValueMeta>);

// ---- WAL 帧头占位（D13）— 注：P5M2 已移除以下占位常量，真实 128 字节帧见 wal/wal_frame.h ----
// 布局：magic:4 | version:1 | flags:1 | entry_type:1 | reserved:1 = 8 字节
inline constexpr std::size_t   kWalFrameHeaderSize = 8;
inline constexpr std::uint32_t kWalMagic   = 0x45424143u;   // "CABE"（字节序在 P5 最终确定）
inline constexpr std::uint8_t  kWalVersion = 1;
inline constexpr std::size_t   kWalOffMagic     = 0;
inline constexpr std::size_t   kWalOffVersion   = 4;
inline constexpr std::size_t   kWalOffFlags     = 5;
inline constexpr std::size_t   kWalOffEntryType = 6;
inline constexpr std::size_t   kWalOffReserved  = 7;

}  // namespace cabe

#endif // CABE_STRUCTS_H
```

---

## 5. 逐项设计

### 5.1 `kValueSize`（D1）

- `inline constexpr std::size_t kValueSize = 1024 * 1024;`
- 取代 `CABE_VALUE_DATA_SIZE`。`std::size_t` 而非宏：类型安全、可用于 `static_assert` 与
  模板参数、有调试信息。
- 命名遵循 ROADMAP 术语表（`kValueSize`，弃用 "chunk" 一词，D4）。

### 5.2 `DeviceId`（D5）

- `using DeviceId = std::uint8_t;` —— 恰好对应 `BlockId` 高 8 位，上限 256 个设备
  （与 N 在 Open 固定、`vector<DeviceContext>` 寻址一致）。

### 5.3 `DataView` / `DataBuffer`（D2 / D4：切 `std::byte`）

- `DataView = std::span<const std::byte>`、`DataBuffer = std::span<std::byte>`。
- 由 `char` 改 `std::byte` 的理由：value 是**裸字节负载**，`std::byte` 在类型上明确
  "这是不透明字节、不是字符/数值"，杜绝误把 value 当字符串处理；与 O_DIRECT / mmap
  缓冲区语义一致。
- **代价**：唯一现存消费者 `crc32.cpp` 需适配遍历方式（§7）。

### 5.4 `BlockId`（D5：8·56 编码）

- 单成员 `struct { std::uint64_t raw; }` 而非裸 `using`：在保持 `sizeof==8`、可平凡复制
  的同时，提供带语义的访问器，杜绝裸 `uint64_t` 的误用（把设备号当偏移等）。
- `Make(dev, idx)`：`(uint64_t(dev) << 56) | (idx & kIdxMask)`；`Make` 对 `block_idx`
  做掩码。为防分配器 bug 传入越界 idx 时静默写错物理块，`Make` 内加一道
  `assert(block_idx <= kIdxMask)`：Debug 防线，`NDEBUG` 下零成本消除；编译期常量求值越界则直接 ill-formed（见 M2-D9）。
- `dev()` = `raw >> 56`；`block_idx()` = `raw & kIdxMask`；`logical_byte_offset()` =
  `block_idx() * kValueSize`（"块号 × 块大小"，不写魔数 `<<20`，随 `kValueSize` 自洽）。
  注：这是**逻辑**偏移；P5 起设备头部 8K 为双份超级块，真实物理偏移 = `kDataRegionOffset + 此值`，由 IoBackend 负责加。
- 提供 defaulted `<=>`：C++20 下它**自动合成** `==` 与全部关系运算（FreeList 升序分配排序、
  一致性相等比较用），无需再显式声明 `==`。
- **溢出边界（注明，非缺陷）**：`logical_byte_offset()` 返回 `uint64_t`，当 `block_idx ≥ 2^44`
  时 `<<20` 会溢出 64 位。对应单设备容量 `2^44 × 1 MiB = 16 EiB`，现实 NVMe 不触及
  （TB–PB 级 = `2^40–2^50` 字节，`block_idx` 远小于 `2^44`）。P1 设备打开时按真实容量
  推导 `block_idx` 上限，天然落在安全区。

### 5.5 `ValueState`（替换 `DataState`）

- `enum class ValueState : std::uint8_t { Active = 0, Deleted = 1 };`
- 取值与旧 `DataState` 完全一致，仅更名（D4 命名分层：数据层用 value 而非 chunk/data）。

### 5.6 `ValueMeta`（24 字节，字段重排）

- 字段：`block`(BlockId) / `timestamp`(uint64) / `crc`(uint32) / `state`(ValueState) /
  `reserved[3]`(uint8×3)。
- **顺序为 `{ block, timestamp, crc, state, reserved }`**（8/8/4/1/3 = 24），自然对齐下
  `sizeof==24`；字段重排的出入及论证见 §3 决策-2 与 §6。
- **`reserved[3] = {}` 把隐式 padding 显式化（M2-D8）**：`ValueMeta` 断言 `is_trivially_copyable`，
  意味着 P5 会 `memcpy` 整体序列化它到 WAL/snapshot。但隐式 padding 在聚合初始化
  （`ValueMeta{block, ts, crc, state}`）下是**不确定值**，会致序列化字节流不确定、CRC 不可复现、
  内存残留泄露。改为带默认成员初始化器 `= {}` 的 `reserved` 字段后，任何初始化路径下这 3 字节
  都确定为 0，并兼作后续小字段（flags 等）的扩展位。
- `= {}` 默认成员初始化器只影响"默认构造是否 trivial"，**不影响** `is_trivially_copyable`
  与 `is_standard_layout`，故 `sizeof==24`、可平凡复制、标准布局全部不变。

### 5.7 WAL 帧头占位常量（D13）

> **P5M2 已取代**：真实 128 字节 WAL 帧（结构 + 编解码 + 常量）落在 `wal/wal_frame.h`；
> 本节描述的 8 字节占位常量（`kWalFrameHeaderSize` / `kWalMagic` / `kWalVersion` / `kWalOff*`）
> 已从 `common/structs.h` 移除。以下为 P0M2 当时的占位设计，留作历史记录。

- M2 只放**布局占位**：`kWalFrameHeaderSize=8`、`kWalMagic`、`kWalVersion` 及各字段偏移
  `kWalOff*`，让 schema 文件先固定帧头尺寸与字段位置。
- 真实编解码、双 CRC（`header_crc`+`payload_crc`）、4 KiB 对齐、`entry_type` 枚举、
  多字节字段端序——全部在 **P5** WAL 设计落地。`kWalMagic` 当前取 `"CABE"` 占位，
  端序待 P5 确定。

### 5.8 旧名映射与删除

| 旧名 | 新名 | 处理 |
|---|---|---|
| `CABE_VALUE_DATA_SIZE` | `cabe::kValueSize` | 删旧、改名 |
| `using BlockId = uint64_t` | `struct cabe::BlockId` | 升级为编码 struct |
| `DataView = span<const char>` | `span<const std::byte>` | 改底层元素类型 |
| `DataBuffer = span<char>` | `span<std::byte>` | 同上 |
| `enum DataState` | `enum ValueState` | 更名 |
| `struct ChunkMeta`（字段 `blockId`） | `struct ValueMeta`（字段 `block`） | 更名 + 字段名/顺序调整 |

- 退出验证：`grep` 全仓确认无 `CABE_VALUE_DATA_SIZE` / `ChunkMeta` / `DataState` /
  `blockId` 残留（`util/util.h` 注释里的 `KeyMeta.createdAt` 一并清理为 `ValueMeta`）。

---

## 6. 内存布局与编译期断言

### 6.1 `ValueMeta` 为何必须重排

**本设计采用的顺序 `{ block, timestamp, crc, state }`**（自然对齐，`align=8`）：

| 字段 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| `block` | `BlockId`(8) | 0 | 8 |
| `timestamp` | `uint64_t` | 8 | 8 |
| `crc` | `uint32_t` | 16 | 4 |
| `state` | `ValueState`(1) | 20 | 1 |
| `reserved[3]` | `std::uint8_t[3]` | 21 | 3 |
| **合计** | | | **24** ✓（无隐式 padding） |

**ROADMAP 字面顺序 `{ block, crc, timestamp, state }`**（对照，说明为何不可行）：

| 字段 | 偏移 | 大小 | 说明 |
|---|---|---|---|
| `block` | 0 | 8 | |
| `crc` | 8 | 4 | |
| (padding) | 12 | 4 | `timestamp` 需 8 对齐 |
| `timestamp` | 16 | 8 | |
| `state` | 24 | 1 | |
| (padding) | 25 | 7 | 结构对齐到 8 |
| **合计** | | | **32** ✗ |

重排省下 8 字节，且不引入 `#pragma pack`（保留自然对齐，避免未对齐访问惩罚）。

### 6.2 断言清单

```cpp
static_assert(sizeof(BlockId) == 8);
static_assert(std::is_trivially_copyable_v<BlockId>);
static_assert(sizeof(ValueMeta)  == 24);
static_assert(alignof(ValueMeta) == 8);
static_assert(std::is_trivially_copyable_v<ValueMeta>);
static_assert(std::is_standard_layout_v<ValueMeta>);
```

- `trivially_copyable` + `standard_layout`：保证 `ValueMeta` 可被 WAL/snapshot 按
  `memcpy` 直接序列化（D3：元数据落 WAL），是 P5 的前置契约。

---

## 7. 连带改动：`DataView` 切 `std::byte` 对 `crc32.cpp` 的波及

`DataView` 元素由 `char` 变 `std::byte` 后，`crc32.cpp` 的两处实现必须适配，否则
`cabe_util` 编译失败。**这是 schema 改造的必然波及，属 M2 交付的一部分**（ROADMAP M2
退出条件虽只提 `structs.h`，但库整体须可构建）。

| 位置 | 现状 | 适配 |
|---|---|---|
| `SoftwareCRC32C` 循环 | `for (const char c : data)` | `for (const std::byte b : data)` |
| 同上，取整 | `static_cast<uint8_t>(c)` | `std::to_integer<std::uint8_t>(b)` |
| `HardwareCRC32C_x86` 指针 | `const char* p = data.data();` | `const std::byte* p = data.data();` |
| 同上，`memcpy` | `memcpy(&chunk, p, 8)` | **不变**（`const void*` 接受 `std::byte*`） |
| 同上，尾字节 | `static_cast<uint8_t>(*p++)` | `std::to_integer<std::uint8_t>(*p++)` |

- `crc32.h` 的 `uint32_t CRC32(DataView)` 签名**不变**（`DataView` 是别名）。
- 算法、查表、运行时分派、`[[gnu::target("sse4.2")]]` 全部不变。
- 验证：M5 的 crc32 已知向量测试会确认 `std::byte` 化未改变计算结果（同一字节序列
  CRC 值不变）。

> 顺带：`util/util.h` 注释中的 `KeyMeta.createdAt`（草稿残留）改为 `ValueMeta` 相关表述，
> 仅注释、无代码语义变化。

---

## 8. 关键设计决策

| # | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| M2-D1 | schema 类型纳入 `namespace cabe` | 保持全局命名空间 | 避免 `DataView`/`BlockId` 全局名冲突；与 `cabe::util` 分层一致 | **建议采纳，待终审**（见 §3 决策-1） |
| M2-D2 | `ValueMeta` 字段重排为 `{block,timestamp,crc,state}` | ROADMAP 字面 `{block,crc,timestamp,state}` | 字面顺序自然对齐为 32 字节；重排是达成 `sizeof==24` 且不破坏对齐的唯一方案 | **建议采纳，待终审**（见 §3 决策-2、§6） |
| M2-D3 | `BlockId` 作单成员 `struct` + 访问器 + `<=>` | 保持裸 `using uint64_t` | 带语义访问、防误用；`sizeof==8`、可平凡复制不变 | 锁定 |
| M2-D4 | `DataView`/`DataBuffer` 切 `std::byte`，连带改 `crc32.cpp` | 保留 `char` | `std::byte` 明确"裸字节"语义，契合裸设备 I/O（D2） | 锁定 |
| M2-D5 | 加 `is_trivially_copyable`/`standard_layout` 断言 | 不加 | 锚定"可 `memcpy` 序列化"契约，为 P5 WAL/snapshot 兜底 | 锁定 |
| M2-D6 | WAL 帧头仅放布局占位常量 | M2 即写编解码 | WAL 模块在 P5，M2 无 WAL 源码 | 锁定 |
| M2-D7 | `logical_byte_offset` 溢出边界仅注明、不加运行期检查 | 加 assert / 饱和 | 现实设备 `block_idx ≪ 2^44`，永不触及；加检查是热路径无谓开销 | 锁定 |
| M2-D8 | `ValueMeta` 末尾隐式 padding 显式化为 `reserved[3] = {}` | 留隐式 padding | 隐式 padding 在聚合初始化下不确定，破坏 memcpy 序列化确定性 / CRC 可复现 / 防信息泄露；并兼作小扩展位 | 锁定（M2 review 修正） |
| M2-D9 | `BlockId::Make` 加 `assert(block_idx <= kIdxMask)` | 不检查（纯调用方责任） | 防分配器 bug 越界静默写错块；Debug 防线、`NDEBUG` 零成本，编译期越界直接 ill-formed | 锁定（M2 review 修正） |
| M2-D10 | `logical_byte_offset` 用 `block_idx() * kValueSize` 而非魔数 `<< 20` | 硬编码 `<< 20` | 消除与 `kValueSize` 脱钩的魔数，语义自解释且自洽（编译器仍优化为移位） | 锁定（M2 review 修正） |

---

## 9. 与 ROADMAP / 决策一致性核对

| ROADMAP M2 / D 决策 | 本设计 | 状态 |
|---|---|---|
| `CABE_VALUE_DATA_SIZE → kValueSize`（D1） | §5.1 | ✅ |
| `DataView`/`DataBuffer` 切 `std::byte`（D4） | §5.3 / §7 | ✅ |
| `BlockId` 8/56 编码 + `Make`/`dev`/`block_idx`/`logical_byte_offset`（D5） | §5.4 | ✅ |
| 新增 `DeviceId=uint8_t`、`enum ValueState` | §5.2 / §5.5 | ✅ |
| 旧 `ChunkMeta`/`DataState` 改名删除 | §5.8 | ✅ |
| `ValueMeta` 24 字节 + `static_assert(==24)` 与 `sizeof(BlockId)==8` | §5.6 / §6 | ⚠️ 达成 24，但**字段顺序与字面不同**（§3 决策-2，待终审） |
| WAL 帧头 8 字节占位常量 | §5.7 | ✅ |
| 命名空间 | 引入 `cabe::` | ⚠️ **超出字面，待终审**（§3 决策-1） |
| 退出：编译通过 / 无旧名残留 / 断言通过 | §11 | ✅ |

---

## 10. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 字段重排引发与"字面顺序"的认知差 | 他人按 ROADMAP 字面预期 `{block,crc,timestamp,state}` | §3/§6 明确论证 + 注释标注；终审锁定后写入 M7 收敛文档 |
| `std::byte` 适配遗漏致编译失败 | `crc32.cpp` 多处 `char` 用法 | §7 列全改动点；双工具链构建 + M5 已知向量回归 |
| `is_trivially_copyable` 被未来字段破坏 | 后续若给 `ValueMeta` 加构造/虚函数 | `static_assert` 常驻，破坏即编译失败 |
| 命名空间引入面 | 若 M3/M4 不跟进，命名空间割裂 | 决策-1 建议作为全工程约定，M3 一并锚定 |
| `kWalMagic` 端序未定 | M2 占位值，P5 才定端序 | 注释标注；M2 不依赖其字节表示 |
| `ValueMeta` 序列化含不确定 padding | 隐式 padding 经 `memcpy` 写入 WAL → 字节流不确定 / CRC 不可复现 / 信息泄露 | 已用显式 `reserved[3] = {}` 消除（M2-D8）；P5 序列化前仍建议整体值初始化 |

---

## 11. 退出条件（DoD）与验证步骤

1. `common/structs.h` 在 GCC 15 / Clang 20 下编译通过（扩展关闭、`-Wall -Wextra -Wpedantic`）。
2. 全部 `static_assert` 通过：`sizeof(BlockId)==8`、`sizeof(ValueMeta)==24`、
   `alignof(ValueMeta)==8`、`is_trivially_copyable`/`standard_layout`。
3. `cabe_util`（含 `std::byte` 适配后的 `crc32.cpp`）双工具链构建通过、零警告。
4. `grep` 全仓无旧名残留：`CABE_VALUE_DATA_SIZE` / `ChunkMeta` / `DataState` /
   `blockId`（字段名）/ `KeyMeta`。
5. `BlockId` 编解码自检（可在 M5 单测固化）：`Make(d,i).dev()==d`、`.block_idx()==i`、
   `.logical_byte_offset()==i<<20`（`i` 在安全区内）。

> **不含**：`ValueMeta`/`BlockId` 的单元测试（M5）、WAL 编解码（P5）。M2 验证为编译期
> 断言 + 双工具链构建 + 无残留 `grep`。

---

## 12. 对下游里程碑的接口承诺

| 里程碑 | M2 提供的接入点 |
|---|---|
| M3 | `error_code`/`logger` 若需类型，可用 `cabe::` 下的 schema；命名空间约定由决策-1 统一锚定 |
| M4 | `Hash(DataView)` 直接复用 `cabe::DataView`(std::byte)；`RouteToDevice` 返回 `DeviceId` |
| M5 | `BlockId`/`ValueMeta` 的断言已就位，M5 补行为单测（编解码往返、字段对齐、enum 取值） |
| P1 | `Engine` 全程使用 `kValueSize`/`BlockId`/`ValueMeta`/`ValueState`；`value.size()==kValueSize` 校验在 Put 路径 |
| P5 | `ValueMeta` 可平凡复制 + `reserved` 已清零（无不确定 padding），可直接 `memcpy` 序列化；WAL 帧头占位常量（`kWal*`，届时可迁入 wal 模块）供写入/恢复落地 |
