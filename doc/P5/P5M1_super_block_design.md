# Cabe P5-M1 设计：设备超级块

> 本里程碑实现设备超级块——每个数据设备关联 WAL 设备 + 快照设备，三块设备通过超级块
> （引擎全局 UUID + 设备 UUID + 双向配对 + 设备编号）关联与校验。引入 create / recover
> 两种启动模式，抽取通用裸设备 I/O 工具 `RawDevice`。超级块采用 **bcache 风格**：三种设备
> 统一在头部 8K 写双份超级块（主 @0 + 备 @4K），数据区 / 环形区 / 快照区从偏移 8K 起；
> 逻辑 block 从 0 编号，数据块物理偏移 = `kDataRegionOffset(8K) + block_idx * kValueSize`，
> 偏移由 IoBackend 加——BlockAllocator 不感知超级块。本里程碑只做超级块与设备布局，
> WAL / 快照 / 恢复在 M2~M6 叠加。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P5 / M1 |
| 状态 | **✅ 已锁定（P5M7 收敛）** |
| 上游依赖 | P3（IoBackend）、P4.5（BlockAllocator）、P5 决策梳理（D1~D11） |
| 下游依赖本里程碑 | P5M2（WAL，复用 RawDevice + 设备布局）、P5M6（恢复，复用超级块校验） |
| 退出判定 | 见 §13 |

---

## 1. 目标与范围

### 1.1 目标

1. 定义统一的 `SuperBlock` 结构（4K，含引擎 UUID、设备 UUID、双向配对、设备编号、自身 CRC32C）。
2. 抽取通用裸设备 I/O 工具 `util/raw_device.*`（打开 + O_DIRECT + 对齐读写 + 对齐缓冲分配）。
3. 实现 `engine/super_block.*`：超级块的 create（写双份）/ recover（读 + 校验 + 主坏用备份修复）。
4. Engine::Open 区分 create / recover 两种模式；recover 做引擎 UUID 一致 + 双向配对 + 设备编号校验。
5. Options 扩展为完整字段清单（三设备路径 + create + WAL/快照配置占位）。
6. 三设备头部 8K 写双份超级块，数据区从偏移 8K 起；逻辑 block 从 0 编号，IoBackend 加数据区偏移。

### 1.2 交付范围

1. **`util/raw_device.h` + `util/raw_device.cpp`**（新建）：裸设备 I/O 工具。
2. **`engine/super_block.h` + `engine/super_block.cpp`**（新建）：SuperBlock 结构 + create/recover/校验逻辑。
3. **`engine/options.h`**（修改）：DeviceConfig 三路径 + Options 完整字段。
4. **`engine/device_context.h`**（修改）：持有本数据设备的 SuperBlock。
5. **`engine/engine.cpp` / `engine.h`**（修改）：Open 区分 create/recover；接入超级块。
6. **`common/structs.h`**（修改）：加超级块布局常量（kSuperBlockSize / kSuperBlockCopies / kDataRegionOffset）。
7. **`io/sync/sync_io_backend.cpp` / `io/uring/io_uring_backend.cpp`**（修改）：Write/Read/BlockCount 加数据区偏移 kDataRegionOffset。
8. **`slots/ring/ring_block_allocator.cpp`**（修改）：Init / RebuildFromActive 保持从 block 0（逻辑块号，不感知超级块）。
9. **`common/error_code.h`**（修改）：kWalRecoveryBase 段加超级块错误码。
10. **`util/CMakeLists.txt` / `engine/CMakeLists.txt`**（修改）：新增源文件。
11. **测试**：`test/util/raw_device_test.cpp` + `test/engine/super_block_test.cpp`；`engine_test` / 契约测试适配。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| WAL 实现 | P5M2 | M1 只做设备布局，WAL 设备此阶段仅写/校验超级块 |
| 快照实现 | P5M4 | 同上 |
| 崩溃恢复（加载快照 + 重放 WAL） | P5M6 | M1 的 recover 只校验超级块；索引重建在 M6 |
| 多设备端到端 | P7 | M1 沿用单设备限制；超级块 device_id 字段为多设备预留 |
| WAL 配置项的实际生效 | P5M3 | M1 在 Options 中占位字段，M3 起生效 |

---

## 2. 现状盘点

- **Options / DeviceConfig**（`engine/options.h`）：当前 `DeviceConfig` 只有 `path`，`Options` 只有 `devices`。
- **IoBackend::Open**（`io/sync/sync_io_backend.cpp`）：`block_count_ = dev_bytes / kValueSize`，物理偏移 = block_idx * kValueSize（P5M1 改为扣除数据区偏移）。
- **BlockAllocator::Init**（`slots/ring/ring_block_allocator.cpp`）：从 block 0 填充（方案 B 下保持不变——逻辑块号，不感知超级块）。
- **错误码**（`common/error_code.h`）：`kWalRecoveryBase = -105000` 段为空，待 M1 填充。
- **Engine::Open**（`engine/engine.cpp`）：当前限制单设备（`opts.devices.size() > 1` 报错），直接 `dc.io.Open` + `dc.block_allocator.Init(0, dc.io.BlockCount())`。
- **`util/raw_device.*` / `engine/super_block.*` 不存在**——本里程碑新建。

---

## 3. 关键决策（已锁定）

| 编号 | 决策 | 结论 |
|---|---|---|
| **P5M1-D1** | 超级块物理布局 | 三设备统一头部双份（主 @0 + 备 @4K，共 8K）；数据区/环形区/快照区从偏移 8K 起（bcache 风格） |
| **P5M1-D2** | 超级块结构 | 统一结构（按 device_type 填充）+ 双向交叉配对校验 |
| **P5M1-D3** | UUID 生成 | 自己生成——`getrandom(2)`（无 fd、阻塞至熵池就绪）；熵源失败则 create 硬失败返回 `kSuperBlockEntropyFailure`，绝不静默退化为低熵/零值；不引入 libuuid |
| **P5M1-D4** | create/recover 判定 | 默认 recover（主备任一通过即可用，主坏备好则用备份覆盖主份；主备都失败退出）；create 显式指定的破坏性操作 |
| **P5M1-D5** | 裸设备 I/O 工具 | 抽取 `util/raw_device.*`，不复用/不改动 IoBackend |
| **P5M1-D6** | Options 字段 | 完整字段清单一次性加全（三路径 + create + WAL/快照配置） |
| **P5M1-D7** | 模块归属与偏移 | 超级块放 `engine/super_block.*`；**IoBackend 加数据区偏移**（`kDataRegionOffset + block_idx * kValueSize`）；BlockAllocator 保持从 block 0（纯逻辑块号，不感知超级块） |
| **P5M1-D8** | 初始化原子性 | 幂等 create + recover 严格校验兜底，不做两阶段提交 |
| **P5M1-D9** | 错误码 | 超级块错误归 `kWalRecoveryBase` 段 |

---

## 4. SuperBlock 结构

```cpp
// engine/super_block.h
#ifndef CABE_SUPER_BLOCK_H
#define CABE_SUPER_BLOCK_H

#include "common/structs.h"   // 实装：kSuperBlockSize / kSuperBlockCopies / kDataRegionOffset 在此（io 与 engine 共享）
#include <cstddef>
#include <cstdint>

namespace cabe {

    // 实装说明：kSuperBlockSize(4K) / kSuperBlockCopies(2) / kDataRegionOffset(8K) 定义在
    // common/structs.h（io 后端需用 kDataRegionOffset 加偏移，故下沉到公共头）；super_block.h
    // 仅保留以下超级块自身常量。
    inline constexpr std::uint32_t kSuperBlockMagic   = 0x43424553u; // "SEBC"
    inline constexpr std::uint32_t kSuperBlockVersion = 1;
    inline constexpr std::size_t   kUuidBytes         = 16;          // 128 位 UUID

    enum class DeviceType : std::uint32_t {
        Data     = 0,
        Wal      = 1,
        Snapshot = 2,
    };

    // 统一超级块结构（4K）。按 device_type 填充相关字段，未用字段留零。
    // standard-layout + trivially-copyable，可直接 memcpy 到 4K 对齐缓冲后写出。
    struct SuperBlock {
        std::uint32_t magic;           // @0    kSuperBlockMagic
        std::uint32_t version;         // @4    格式版本
        std::uint32_t device_type;     // @8    DeviceType
        std::uint32_t reserved0;       // @12   对齐
        std::uint8_t  engine_uuid[16];        // @16   引擎全局 UUID（三设备共享）
        std::uint8_t  device_uuid[16];        // @32   本设备唯一 UUID
        std::uint8_t  paired_data_uuid[16];   // @48   所属数据设备 UUID（WAL/快照填）
        std::uint8_t  paired_wal_uuid[16];    // @64   配对 WAL UUID（数据设备填）
        std::uint8_t  paired_snapshot_uuid[16];//@80  配对快照 UUID（数据设备填）
        std::uint64_t device_id;       // @96   数据设备编号（多设备顺序校验，M1 下为 0）
        std::uint64_t block_count;     // @104  数据设备可用块数（数据设备填）
        std::uint64_t created_at;      // @112  创建时间戳（util::GetWallTimeNs）
        std::uint8_t  reserved[3972];  // @120  预留扩展，填零
        std::uint32_t crc32c;          // @4092 自身 CRC32C，覆盖 [0, 4092)
    };

    static_assert(sizeof(SuperBlock) == kSuperBlockSize);
    static_assert(std::is_standard_layout_v<SuperBlock>);
    static_assert(std::is_trivially_copyable_v<SuperBlock>);

} // namespace cabe

#endif // CABE_SUPER_BLOCK_H
```

**字段说明**：
- `magic`：识别 cabe 超级块；recover 时魔数不符即判定无效超级块。
- `engine_uuid`：标识整个引擎实例，三块设备共享——校验"属于同一实例"。
- `device_uuid`：每块设备唯一——配对校验的锚点。
- `paired_*`：双向配对——数据设备记录配对的 WAL/快照 UUID，WAL/快照记录所属数据设备 UUID。
- `device_id`：数据设备在 `Options.devices` 中的序号——防盘符漂移（M1 单设备恒为 0，P7 多设备生效）。
- `crc32c`：覆盖前 4092 字节，校验超级块自身完整性。

---

## 5. 物理布局

三种设备统一布局——头部 8K 双份超级块，数据区/环形区/快照区从偏移 8K 起：

```
数据设备：
┌──────────┬──────────┬───────────┬───────────┬───────────┐
│超级块主份│超级块备份│ block 0   │ block 1   │ block 2   │ ...
│  @0 4K   │ @4K 4K   │ 数据块    │ 数据块    │ 数据块    │
└──────────┴──────────┴───────────┴───────────┴───────────┘
0          4K         8K          8K+1M       8K+2M
|<-- 头部 8K 超级块 -->|<--------- 数据区 ----------->|

WAL 设备 / 快照设备：
┌──────────┬──────────┬─────────────────────────────────────┐
│超级块主份│超级块备份│  环形区 / 快照区 → 设备尾            │
│  @0 4K   │ @4K 4K   │                                     │
└──────────┴──────────┴─────────────────────────────────────┘
0          4K         8K                                设备尾
```

- 三设备头部 8K（`kDataRegionOffset`）为双份超级块（主 @0 + 备 @4K），其后是数据区/环形区/快照区。
- 数据设备：逻辑 block 从 0 编号，**物理偏移 = `kDataRegionOffset + block_idx * kValueSize`**（block 0 物理偏移 = 8K）。偏移由 IoBackend 加，BlockAllocator 只管逻辑块号。
- 可分配数据块数 = `(dev_bytes - kDataRegionOffset) / kValueSize`。
- 数据块物理位置（8K + N×1M）对齐到 4K，满足 O_DIRECT；1M 大块在 NVMe 上对齐到 4K 还是 1M 性能差异可忽略。

---

## 6. RawDevice 工具

```cpp
// util/raw_device.h
namespace cabe {

    // 通用裸设备 I/O：打开 + O_DIRECT + 设备大小查询 + 任意偏移/长度的对齐读写。
    // 服务于超级块（4K）、WAL（4K 帧）、快照（流式）——与 IoBackend 的 1M 块语义分离。
    class RawDevice {
    public:
        RawDevice() noexcept = default;
        ~RawDevice();
        RawDevice(const RawDevice&) = delete;
        RawDevice& operator=(const RawDevice&) = delete;
        RawDevice(RawDevice&&) noexcept;
        RawDevice& operator=(RawDevice&&) noexcept;

        int32_t Open(const std::string& path);            // O_RDWR | O_DIRECT
        int32_t Close();
        std::uint64_t SizeBytes() const noexcept;          // ioctl(BLKGETSIZE64)
        // offset 与 len 必须 4K 对齐；buf 必须 4K 对齐（O_DIRECT 约束）
        int32_t ReadAt(std::uint64_t offset, std::byte* buf, std::size_t len);
        int32_t WriteAt(std::uint64_t offset, const std::byte* buf, std::size_t len);
        bool is_open() const noexcept;

        // 4K 对齐缓冲分配（posix_memalign），调用方用完 FreeAligned 释放
        static std::byte* AllocAligned(std::size_t size);
        static void FreeAligned(std::byte* p) noexcept;

    private:
        int fd_ = -1;
        std::uint64_t size_bytes_ = 0;
    };

} // namespace cabe
```

要点：
- `ReadAt` / `WriteAt` 的 offset、len、buf 都要求 4K 对齐——O_DIRECT 硬约束。超级块 4K、WAL 帧凑 4K、快照大块都满足。
- 放 `util/`——它是无业务语义的基础工具，util 模块依赖最少。
- RawDevice 服务于超级块的 4K 粒度读写；数据设备的 1M 块 I/O 仍由 IoBackend 负责（方案 B 下 IoBackend 内部加 `kDataRegionOffset` 偏移）。

---

## 7. create / recover 流程

### 7.1 create（破坏性初始化，用户显式 `opts.create = true`）

```
对每个设备组 cfg（M1 下只有一组）：
  1. 打开三块设备（RawDevice 临时句柄）+ 校验大小
     - 数据设备：必须 ≥ kDataRegionOffset + kValueSize（8K 超级块 + 至少 1 个数据块）
     - WAL / 快照设备：必须 ≥ kDataRegionOffset（8K，双份超级块）
       （注：快照 / WAL 设备的部署期容量下界校验在 P5M4 引入——见 P5M4 §11；此处 M1 只保证 ≥8K 能放下超级块）
  2. 生成引擎全局 UUID + 三设备 device_uuid（getrandom(2)）；
     任一失败 → 整体 create 失败，返回 kSuperBlockEntropyFailure，不写任何超级块
  3. 填充三个 SuperBlock（数据设备记 paired_wal/snapshot + device_id + block_count；
     block_count = (数据设备字节数 - kDataRegionOffset) / kValueSize；WAL/快照记 paired_data），各算 CRC32C
  4. 各写双份并持久化：每设备先写备份 @4K + fdatasync，再写主份 @0 + fdatasync；
     跨设备写序为 WAL → 快照 → 数据（数据设备最后，作为 recover 锚点）
  5. Init BlockAllocator（从 block 0，可分配数 = block_count）
  6. DeviceContext 保存数据设备的 SuperBlock
```

幂等性 + 崩溃安全（D8）：create 是覆盖写，中途崩溃后重新 create 全部重写——无需两阶段提交。
崩溃安全由"写序 + 每份 fdatasync"提供：设备内主份只在完整备份持久后才出现，跨设备数据设备最后持久；
故任意断电点要么是完整的旧/新组、要么数据设备被判为"未格式化"——不会留下可被误认为有效的半成品。
注意：崩溃的 create 不会被 recover 自愈（recover 严格校验后失败），需重跑 create。

### 7.2 recover（默认，`opts.create = false`）

```
对每个设备组 cfg：
  1. 打开三块设备 + 校验最小尺寸（与 create 对称：数据 ≥ 8K+1块；WAL/快照 ≥ 8K，否则 kEngineInvalidOpts）
  2. 读各设备超级块（CheckSuperBlock = 魔数 + 版本 + CRC 三项）：
     - 读主份 @0：通过 → 用主份
       失败 → 读备份 @4K：
         通过 → 用备份，并用备份覆盖主份（修复，恢复双份冗余；修复写失败仅 WARN，不致 recover 失败）
         失败 → 退出，按失败类型返回：
           主备读 I/O 均失败（坏道/EIO）→ kSuperBlockReadFailed（区别于"未格式化"，勿据此重建）
           主备都"魔数/版本不符"（未格式化/插错盘/格式不兼容）→ kSuperBlockMagicMismatch
           存在本格式但 CRC 损坏                              → kSuperBlockCrcMismatch
           （兜底 kSuperBlockNoValid）
  3. 校验：
     - 设备类型匹配：data/wal/snap 的 device_type 各为 Data/Wal/Snapshot
         → 否则 kSuperBlockDeviceTypeMismatch
     - 三设备 engine_uuid 一致         → 否则 kSuperBlockEngineUuidMismatch
     - 双向配对：
         数据.paired_wal_uuid == WAL.device_uuid 且 WAL.paired_data_uuid == 数据.device_uuid
         数据.paired_snapshot_uuid == 快照.device_uuid 且 快照.paired_data_uuid == 数据.device_uuid
         → 否则 kSuperBlockPairMismatch
     - 三设备 device_id == 在 devices 中的序号（M1 下为 0，三设备对称校验）
         → 否则 kSuperBlockDeviceIdMismatch
  4. block_count 核对：持久 block_count 与当前数据设备实际 (SizeBytes-8K)/kValueSize 比较；
     持久 > 实际（设备被缩容）→ kSuperBlockSizeMismatch；持久 < 实际（扩容）→ 保留持久值、不自动扩
  5. 任一校验失败 → 整体 Open 失败退出，不做自动 create
  5. Init BlockAllocator（从 block 0，block_count 取自超级块）—— M1 阶段索引为空，全部空闲
  6. DeviceContext 保存数据设备的 SuperBlock
```

**M1 里程碑衔接**：M1 阶段 recover 只校验超级块，索引仍为空（WAL/快照恢复在 M6）。M1 重启后数据不持久——正常，M1 只验证超级块机制。

---

## 8. Options 扩展

```cpp
// engine/options.h
namespace cabe {

    enum class WalLevel : std::uint8_t {
        Strict    = 1,  // value 落盘 + WAL 落盘 + 内存 → 返回
        ValueSync = 2,  // value 落盘 + 内存 → 返回（WAL 攒批异步）
        WalSync   = 3,  // WAL 落盘 + 内存 → 返回（value 异步）—— 默认
        Async     = 4,  // 仅内存 → 返回（全异步）
    };

    struct DeviceConfig {
        std::string data_path;       // 数据设备（裸块设备，存 value）
        std::string wal_path;        // WAL 设备（裸块设备，存元数据日志）
        std::string snapshot_path;   // 快照设备（裸块设备，存索引镜像）
    };

    struct Options {
        std::vector<DeviceConfig> devices;            // 设备组，N = size()

        bool create = false;                          // false=recover（默认）；true=create（破坏性初始化）

        // WAL 配置（全局统一；M3 起生效，M1 占位）
        WalLevel wal_level = WalLevel::WalSync;        // 默认级别 3
        std::size_t wal_buffer_size = 32 * 1024;       // 攒批缓冲，默认 32K（P5M3：同步/攒批共用单块、Open 时定死，运行时改大小留未来）
        std::uint32_t wal_flush_interval_ms = 1000;    // 定时刷出兜底，默认 1s（P5M3 不读；定时刷出需后台线程，推迟 P7）

        // 快照配置（全局统一；M4 起生效，M1 占位）
        std::uint64_t snapshot_threshold_bytes = 512ull * 1024 * 1024; // WAL 达 512M 触发快照
        // （P5M4 新增 snapshot_buffer_size = 1M：快照流式写的临时缓冲，每次快照现读、可动态改——
        //   M1 的"完整字段清单"未含此项，M4 按需补入快照配置块）
        std::uint32_t snapshot_interval_sec = 600;     // 定时快照兜底，默认 10 分钟（P5M4 注：M4 不读、P7 起生效）
                                                       // 触发 = 大小阈值 OR 定时，任一满足

        // 恢复配置（M6 起生效，M1 占位）
        bool verify_value_crc_on_recovery = false;     // 恢复时是否逐个校验 value CRC，默认关
    };

} // namespace cabe
```

注：M1 只让 `devices`（三路径）+ `create` 生效，其余字段定义但暂不参与逻辑——一次性加全避免后续里程碑遗漏（P5-D6）。

---

## 9. Engine / DeviceContext / BlockAllocator 改造

### 9.1 DeviceContext

```diff
  struct DeviceContext {
      IoBackendImpl io;
      BufferPool pool{0};
      BlockAllocatorImpl block_allocator;
      MetaIndexImpl meta_index;
+     SuperBlock super_block;   // 本数据设备的超级块（recover 读入 / create 写入后保存）
  };
```

WAL / 快照设备的常驻句柄推到 M2 / M4——M1 用临时 RawDevice 完成超级块读写后关闭。

### 9.2 Engine::Open

```cpp
Status Engine::Open(const Options& opts) {
    if (opened_) return Status::Error(err::kEngineAlreadyOpen);
    if (opts.devices.empty()) return Status::Error(err::kEngineInvalidOpts);
    if (opts.devices.size() > 1) return Status::Error(err::kEngineInvalidOpts);  // M1 沿用单设备

    for (std::size_t i = 0; i < opts.devices.size(); ++i) {
        const auto& cfg = opts.devices[i];
        DeviceContext dc;

        // 超级块：create 写双份 / recover 读校验（内部用 RawDevice 读写三设备头部 8K）
        int32_t rc = opts.create
            ? CreateDeviceGroup(cfg, i, &dc.super_block)
            : RecoverDeviceGroup(cfg, i, &dc.super_block);
        if (rc != err::kSuccess) { /* 清理 + 返回 */ }

        // 打开数据设备的 IoBackend（其 Write/Read 内部加 kDataRegionOffset）
        rc = dc.io.Open(cfg.data_path);
        if (rc != err::kSuccess) { /* 清理 + 返回 */ }

        dc.pool = BufferPool(kDefaultPoolBlocks);
        // 用超级块记录的 block_count（数据区块数，权威值）；Init 从 block 0
        dc.block_allocator.Init(0, dc.super_block.block_count);
        devices_.push_back(std::move(dc));
    }

    opened_ = true;
    return Status::Ok();
}
```

`CreateDeviceGroup` / `RecoverDeviceGroup` 为 `engine/super_block.*` 的自由函数，内部用 RawDevice 读写三设备头部 8K 的双份超级块。超级块读写走 RawDevice（4K 粒度），不经过 IoBackend。

### 9.3 IoBackend 加数据区偏移 + BlockAllocator 保持 block 0

**IoBackend**（sync 与 io_uring 同样改）——Write / Read 的物理偏移加 `kDataRegionOffset`，BlockCount 扣除超级块区：

```diff
  // Open：
- block_count_ = dev_bytes / kValueSize;
+ block_count_ = (dev_bytes - kDataRegionOffset) / kValueSize;  // 扣除头部 8K 超级块区

  // Write / Read：
- const auto offset = block_idx * kValueSize;
+ const auto offset = kDataRegionOffset + block_idx * kValueSize;  // 数据区从 8K 起
```

**BlockAllocator**——保持从 block 0 填充（方案 B 下不感知超级块，逻辑块号即数据区块号）：

```cpp
// Init / RebuildFromActive 保持：
for (std::uint64_t i = 0; i < block_count; ++i) { ... }
```

逻辑块 0 ~ block_count-1 由 BlockAllocator 管理；IoBackend 把逻辑块号映射到物理偏移（加 8K）。两者职责分离——BlockAllocator 纯数据结构，超级块感知收归 IoBackend。

---

## 10. 错误码

`common/error_code.h` 的 `kWalRecoveryBase`（-105000）段新增：

```cpp
inline constexpr int kSuperBlockMagicMismatch      = InSeg(kWalRecoveryBase, 0); // -105000 主备都非本格式/未格式化（魔数或版本不符）
inline constexpr int kSuperBlockCrcMismatch        = InSeg(kWalRecoveryBase, 1); // -105001 本格式但主备 CRC 均损坏
inline constexpr int kSuperBlockEngineUuidMismatch = InSeg(kWalRecoveryBase, 2); // -105002
inline constexpr int kSuperBlockPairMismatch       = InSeg(kWalRecoveryBase, 3); // -105003
inline constexpr int kSuperBlockDeviceIdMismatch   = InSeg(kWalRecoveryBase, 4); // -105004
inline constexpr int kSuperBlockDeviceTypeMismatch = InSeg(kWalRecoveryBase, 5); // -105005 设备类型不符（如把 WAL 盘当数据盘）
inline constexpr int kSuperBlockNoValid            = InSeg(kWalRecoveryBase, 6); // -105006 兜底：主备都无效但无法细分
inline constexpr int kSuperBlockReadFailed         = InSeg(kWalRecoveryBase, 7); // -105007 主备均读 I/O 失败（区别于未格式化，勿据此重建）
inline constexpr int kSuperBlockEntropyFailure     = InSeg(kWalRecoveryBase, 8); // -105008 系统熵源不可用，create 中止
inline constexpr int kSuperBlockSizeMismatch       = InSeg(kWalRecoveryBase, 9); // -105009 持久 block_count 与当前设备大小不符（缩容）
```

- 版本号（`version`）不符与魔数不符同归 `kSuperBlockMagicMismatch`——均视为"非本格式"，防止未来 v2 超级块被旧二进制按错误语义读取而毁数据。
- `kSuperBlockReadFailed` 与 `kSuperBlockMagicMismatch` 必须区分：前者是读 I/O 失败（盘可能有数据、可恢复），后者是"未格式化/插错盘"；混淆会诱导运维对有数据的坏盘重建而毁数据。
- `kSuperBlockEntropyFailure` 由 create 期 getrandom 失败触发（安全攸关，硬失败而非降级）。
- RawDevice 的 I/O 错误复用 `kIoBase` 段。

---

## 11. 目录与 CMake

```
util/
├── raw_device.h            # 新建
└── raw_device.cpp          # 新建

engine/
├── super_block.h           # 新建
└── super_block.cpp         # 新建

test/util/raw_device_test.cpp        # 新建
test/engine/super_block_test.cpp     # 新建
```

- `util/CMakeLists.txt`：加 `raw_device.cpp` 到 `cabe_util`。
- `engine/CMakeLists.txt`：加 `super_block.cpp` 到 `cabe_engine`。
- `test/CMakeLists.txt`：注册两个新测试。

---

## 12. 测试设计

### 12.1 RawDevice 测试（需 loop 设备）

| 用例 | 验证 |
|---|---|
| `OpenCloseNormal` | 打开 / 关闭 / is_open |
| `SizeBytesCorrect` | SizeBytes 返回设备字节数 |
| `WriteReadAlignedRoundTrip` | 4K 对齐写 → 读回比对一致 |
| `AllocAlignedIs4KAligned` | AllocAligned 返回 4K 对齐指针 |
| `OpenBadPath` | 不存在路径 → 错误 |

### 12.2 超级块测试（需 3 个 loop 设备：数据 + WAL + 快照）

| 用例 | 验证 |
|---|---|
| `CreateThenRecover` | create 写超级块 → recover 校验通过 |
| `RecoverWithoutCreateFails` | 全新设备直接 recover → kSuperBlockMagicMismatch / kSuperBlockNoValid |
| `CrcCorruptionDetected` | 篡改主份 CRC → recover 用备份成功 + 主份被修复 |
| `BothCorruptedFails` | 主备都篡改 → recover 退出 |
| `EngineUuidMismatch` | 把另一实例的 WAL 设备配进来 → kSuperBlockEngineUuidMismatch |
| `PairMismatch` | 配错 WAL/快照设备 → kSuperBlockPairMismatch |
| `CreateIdempotent` | 重复 create → 覆盖成功，recover 仍通过 |
| `RecoverDeviceIdMismatch` | create(device_id=0) 后用 device_id=1 recover → kSuperBlockDeviceIdMismatch |

注：BlockAllocator 从 block 0 起的行为由块分配器契约测试（`test_block_allocator_contract`）覆盖；
逻辑 block 0 经 IoBackend 映射到物理偏移 8K，端到端正确性由 `engine_test`（含 CRC32 篡改物理偏移 8K）验证。

---

## 13. 退出条件

1. `SuperBlock` 结构 + `static_assert`（4K、standard-layout、trivially-copyable）通过。
2. `RawDevice` 实装 + 对齐读写测试全绿。
3. `engine/super_block.*` 实装：create 写双份、recover 读校验 + 主坏用备份修复。
4. Engine::Open 区分 create/recover，双向配对 + 设备编号校验生效。
5. IoBackend（sync + io_uring）Write/Read 加 kDataRegionOffset 偏移、BlockCount 扣除超级块区；BlockAllocator 保持从 block 0。
6. Options 完整字段就位（M1 仅 devices + create 生效）。
7. 超级块错误码归 kWalRecoveryBase 段。
8. 超级块测试全绿（需 3 个 loop 设备）；现有测试不退步。
9. 四档（release / asan / tsan / ubsan）全绿；覆盖率 ≥ 80%（sync 后端）。

---

## 14. 对下游里程碑的接口承诺

| 下游 | 接入点 |
|---|---|
| **P5M2（WAL）** | 复用 `RawDevice` 读写 WAL 设备；WAL 环形区从 @8K 起；超级块已校验设备身份 |
| **P5M4（快照）** | 复用 `RawDevice`；快照区从 @8K 起 |
| **P5M6（恢复）** | recover 流程已搭好超级块校验骨架，M6 在校验通过后追加"加载快照 + 重放 WAL + 重建 BlockAllocator" |
| **P7（多设备）** | 超级块 device_id 字段已就位，多设备时校验顺序；解除单设备限制 |

---

**全文完。**
