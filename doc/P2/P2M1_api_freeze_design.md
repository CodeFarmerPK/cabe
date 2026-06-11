# Cabe P2-M1 设计：公开 API 审查 + 冻结声明

> 本里程碑审查 P1 已实装的全部公开接口（Engine / Options / Status / 错误码），确认能撑到
> 项目完工，然后输出**公开 API 符号清单 + 冻结声明**。冻结为设计意图声明（尽量保持不改），
> 非绝对约束——未来有需要时可改，改了同步更新文档。
>
> **本文即审查结论 + 冻结声明**。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P2 / M1 |
| 状态 | **✅ 已锁定（P2M2 收敛）** |
| 上游依赖 | P1 全部完成（Engine 骨架 + BufferPool + I/O + FreeList + MetaIndex + Put/Get/Delete 端到端） |
| 下游依赖本里程碑 | P2M2（收敛）；P3+ 所有后续阶段以此为公开 API 基准 |
| 退出判定 | 见 §8 |

---

## 1. 冻结总原则

**冻结 = 设计意图声明**：

- 当前阶段（全部完工前）：尽量保证后续开发不改公开接口；如果被迫要改，改了同步更新文档
- 全部完工发布后：严格约束——改接口等同 v2.0
- 本文列出的所有"冻结"条目均按此原则理解，不再逐条重复

---

## 2. 决策汇总

| 编号 | 决策 | 状态 |
|---|---|---|
| **P2M1-D1** | API 版本号仅文档承诺（CMake `project(VERSION)` + ROADMAP），不进 Options / Status 运行时字段 | 锁定 |
| **P2M1-D2** | 错误码尽量保持：段基址 / 容量 / 已分配码值当前固化，但非绝对约束——未来引入新模块可追加或调整 | 锁定 |
| **P2M1-D3** | 函数签名尽量保持；Status 布局尽量保持（4 字节不加字段）；Options / DeviceConfig 可追加字段；枚举只追加不改已有值 | 锁定 |
| **P2M1-D4** | Engine 析构语义确认：未 Close 直接析构 → 自动 Close + 日志警告 | 锁定 |
| **P2M1-D5** | Put 持久化原子性由 WAL 保证（P5+）；当前阶段（P1-P4）无持久化保证 | 锁定 |
| **P2M1-D6** | Status 薄包装 `{ int code; }` + `ok()` + `explicit operator bool()` 确认，不加字段 | 锁定 |

---

## 3. 公开 API 符号清单

以下类型 / 方法 / 常量对外公开，后续尽量保持不变。

### 3.1 公开类型

#### `cabe::Engine`（`engine/engine.h`）

```cpp
class Engine {
public:
    Engine() noexcept = default;
    ~Engine();                                          // 自动 Close + 日志警告

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    Status Open(const Options& opts);                   // Closed → Opened
    Status Close();                                     // Opened → Closed

    Status Put(std::string_view key, DataView value);   // value.size() == kValueSize
    Status Get(std::string_view key, DataBuffer value); // value.size() == kValueSize
    Status Delete(std::string_view key);

    Status SetWalLevel(WalLevel level);                 // P5M3 起新增：运行时改 WAL 级别
    Status Snapshot();                                  // P5M4 起新增：手动触发一份快照（同步返结果）

    bool is_open() const noexcept;
};
```

> **冻结追加注（P5）**：`SetWalLevel`（P5M3）与 `Snapshot()`（P5M4）为冻结后按 P5-D5
> "破坏冻结、同步更新文档"约定追加的公开方法。`Snapshot()` 在 recover 模式（M4 未打开
> 快照设备）返回 `kEngineNotImplemented`（恢复编排在 P5M6）。

**承诺语义**：

| 方法 | 承诺 |
|---|---|
| `Open` | 已 Opened 时返回 `kEngineAlreadyOpen`（幂等防护）；P1 限单 device，未来多 device 时 `opts.devices.size() > 1` 将被支持 |
| `Close` | 未 Opened 时返回 `kEngineNotOpen`；释放所有资源 |
| `Put` | key 非空 + `value.size() == kValueSize`；覆盖写（同 key 最后一次 Put 生效）；**持久化原子性由 WAL 保证（P5+），当前无持久化保证**。P5M5 起可能返回 `kWalFull`（仅 WAL 环空间耗尽且快照救援无效时——失败干净：索引未动、旧值可读；属运维信号） |
| `Get` | 返回最后一次 Put 的 value；CRC32 校验不匹配返回 `kEngineDataCorrupted` |
| `Delete` | 标记删除 + 立即回收块号；删后 Get 返回 `kIndexKeyNotFound`。P5M5 起可能返回 `kWalFull`（同 Put：失败时索引/块全不动） |
| 析构 | 若仍 Opened → 自动 Close + `CABE_LOG_WARN` |

#### `cabe::Options`（`engine/options.h`）

```cpp
struct DeviceConfig {
    std::string data_path;       // 数据设备（裸块设备，存 value）
    std::string wal_path;        // WAL 设备（P5 起）
    std::string snapshot_path;   // 快照设备（P5 起）
};

struct Options {
    std::vector<DeviceConfig> devices;  // 设备组列表，N 在 Open 时固定（D8）
    bool create = false;                // P5：false=recover（默认）/ true=create（破坏性初始化）
    // P5 WAL / 快照 / 恢复配置（全局统一，末尾追加）：
    WalLevel wal_level = WalLevel::WalSync;
    std::size_t wal_buffer_size = 32 * 1024;
    std::uint32_t wal_flush_interval_ms = 1000;
    std::uint64_t snapshot_threshold_bytes = 512ull * 1024 * 1024;
    std::size_t   snapshot_buffer_size     = 1024 * 1024;   // P5M4 新增。注：按主题插入快照配置块内（threshold 与 interval 之间），
                                                            //   未严格"末尾追加"——带默认值、不改已有字段类型/含义，但移动了后续字段位置；
                                                            //   cabe 不承诺 ABI 且 Options 不用位置式聚合初始化，故可接受（合约定精神）
    std::uint32_t snapshot_interval_sec = 600;
    bool verify_value_crc_on_recovery = false;
};
```

**可扩展约定**：Options / DeviceConfig 可在末尾追加新字段（带默认值），不改已有字段的类型 / 含义 / 位置。

#### `cabe::Status`（`engine/status.h`）

```cpp
struct Status {
    int code = err::kSuccess;           // 4 字节，trivially_copyable

    constexpr bool ok() const noexcept;
    constexpr explicit operator bool() const noexcept;
    static constexpr Status Ok() noexcept;
    static constexpr Status Error(int c) noexcept;
    constexpr auto operator<=>(const Status&) const noexcept = default;
};
```

**布局冻结**：`sizeof(Status) == sizeof(int)` + `trivially_copyable`——尽量保持不加字段。

### 3.2 公开常量

| 常量 | 值 | 头文件 | 含义 |
|---|---|---|---|
| `kValueSize` | `1048576`（1 MiB） | `common/structs.h` | 定长 value 大小（D1 锁定） |

### 3.3 公开结构体（数据层）

| 类型 | 头文件 | 用途 |
|---|---|---|
| `BlockId` | `common/structs.h` | 物理块地址（8 字节，D5 编码） |
| `DeviceId` | `common/structs.h` | 设备编号（`uint8_t`） |
| `DataView` | `common/structs.h` | 只读字节视图（`std::span<const std::byte>`） |
| `DataBuffer` | `common/structs.h` | 可写字节视图（`std::span<std::byte>`） |
| `ValueMeta` | `common/structs.h` | value 元数据（24 字节，RAM 索引条目） |
| `ValueState` | `common/structs.h` | `Active = 0 / Deleted = 1` |

### 3.4 内部类型（不在冻结承诺内）

以下类型为 Engine 内部实现，不对外承诺稳定：

| 类型 | 说明 |
|---|---|
| `DeviceContext` | Engine 内部运行时状态（fd + pool + free_list + meta_index） |
| `BufferPool` | 对齐 buffer 池 |
| `FreeList` | 块号空间管理 |
| `MetaIndex` | RAM 索引 |
| `WriteBlock` / `ReadBlock` | 朴素 I/O 辅助函数 |

---

## 4. 错误码空间审查

### 4.1 段位划分（尽量保持）

| 段 | 基址 | 范围 | 当前已分配 |
|---|---|---|---|
| memory | -100000 | [-100999, -100000] | 4 个 |
| io | -101000 | [-101999, -101000] | 0 个（P3+ 填） |
| index | -102000 | [-102999, -102000] | 1 个 |
| wal | -103000 | [-103999, -103000] | 0 个（P5+ 填） |
| engine | -104000 | [-104999, -104000] | 8 个 |
| wal_recovery | -105000 | [-105999, -105000] | 0 个（P5+ 填） |

**总容量**：6 × 1000 = 6000 个码——当前仅用 13 个——后续阶段充足。

### 4.2 已分配码值清单

| 码 | 值 | 段 |
|---|---|---|
| `kSuccess` | 0 | — |
| `kMemNullPointer` | -100000 | memory |
| `kMemEmptyKey` | -100001 | memory |
| `kMemEmptyValue` | -100002 | memory |
| `kMemInsertFail` | -100003 | memory |
| `kEngineAlreadyOpen` | -104000 | engine |
| `kEngineNotOpen` | -104001 | engine |
| `kEngineInvalidOpts` | -104002 | engine |
| `kEngineInvalidValue` | -104003 | engine |
| `kEngineNotImplemented` | -104004 | engine |
| `kEngineNoSpace` | -104005 | engine |
| `kEnginePoolExhausted` | -104006 | engine |
| `kEngineDataCorrupted` | -104007 | engine |
| `kIndexKeyNotFound` | -102000 | index |

### 4.3 扩展约定

- 新码在对应段内追加（如 `InSeg(kIoBase, 0)` / `InSeg(kWalBase, 0)` 等）
- 已分配码值尽量不改——但非绝对约束；如果未来引入新模块需要调整，改了同步更新文档
- 段基址和段容量（1000）尽量保持
- 如果 6 段不够用，可在 `-106000` 起新增段——不破坏已有段

---

## 5. 返回值分层约定（审查确认）

P1 期间确立的约定，P2 冻结确认：

| 层 | 返回类型 | 示例 |
|---|---|---|
| 公开 API（Engine 5 个方法） | `cabe::Status` | `return Status::Ok();` |
| 内部组件（IO / FreeList / MetaIndex） | `int32_t` 错误码 | `return err::kSuccess;` |
| 转换点 | Engine 方法体内 | `if (rc != err::kSuccess) return Status::Error(rc);` |

---

## 6. 不在冻结范围的内容

| 内容 | 原因 | 何时定型 |
|---|---|---|
| 零拷贝 `BufferHandle` 接口 | 尚未实装 | P8 |
| 多 device 路由策略 | P1 写死单 device | P3（IoBackend 抽象层引入时） |
| WAL 帧格式 / recovery 接口 | 尚未实装 | P5 |
| Reactor 并发接口 | 尚未实装 | P7 |
| `IoBackend` / `MetaIndex` concept | 尚未引入抽象层 | P3 |

---

## 7. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 冻结太早 | P3-P12 可能发现公开 API 不够用——需要改签名 | 冻结是意图声明非绝对约束；改了同步更新文档 |
| Options 扩展 | P5 已按"末尾追加"约定扩展：`DeviceConfig` 改三路径（data/wal/snapshot）、`Options` 增 `create` + WAL/快照/恢复字段 | 追加字段未改既有字段语义；DeviceConfig 由单路径变三路径属被迫改动，已同步本文 |
| 错误码扩展 | P5M1 已在 wal_recovery 段分配超级块校验码（`kSuperBlock*`，-105000 起）；io / wal 段仍按需追加 | 6000 容量充足；段内追加即可 |
| Put 无持久化保证 | 当前 power loss 丢全部数据 | P5 WAL 解决；API 承诺语义已声明"P5+ 才有持久化原子性" |

---

## 8. 退出条件

1. **本设计稿完成**：公开 API 符号清单（§3）+ 错误码清单（§4）+ 冻结总原则（§1）输出。
2. **owner 审阅通过**：确认符号清单完整、承诺语义准确、冻结原则接受。
3. **无代码改动**：P2M1 纯文档——不改 engine / common / test 代码。

---

## 9. 对下游里程碑的接口承诺

| 下游 | 本里程碑提供的接入点 |
|---|---|
| **P2M2** | 本文件作为 P2 收敛的依据 |
| **P3+** | §3 公开 API 符号清单 = 后续阶段的"不改基线"；改了需回到本文更新 |
| **P5** | §5 Put 持久化承诺 = WAL 设计的约束输入 |

---

**全文完。**
