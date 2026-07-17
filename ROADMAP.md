# Cabe 项目路线图 (v4)

> 本文件描述本项目的完整演进阶段。每阶段只描述核心目标和大体范围,具体里程碑、决策、风险见各阶段独立设计稿 `doc/PN/PNMn_<主题>_design.md`（每阶段一个目录，索引在 `doc/PN/README.md`）。
>
> 本版整合多轮设计讨论后的全部架构决策,自 v4 起作为正式路线。

---

## 一、项目定位

定长 value 键值存储引擎,直接操作 NVMe 裸块设备。

**对外契约**:
- 按 `key` 把**恰好 `kValueSize`(1 MiB)字节**的数据存到 NVMe 设备上
- 持久化、可恢复、并发安全
- 单机部署,可多 NVMe 聚合带宽

**形态**:
- 公开 API:同步调用,内部按需异步
- 部署:Linux 用户态进程；sync / `io_uring` 使用块设备节点，P9 SPDK 使用显式 `BDF + namespace id + 可选字节区间` 直接访问 NVMe namespace
- 设备数 N 在 Open 时固定,运行期不可变

---

## 二、永久排除 / 延后但未排除

### 永久排除(项目边界外)

- 大 value 切分、value 聚合(交由上层负责)
- 变长 value(必须恰好 `kValueSize`)
- 对象存储 API、文件系统语义
- 范围扫描(range scan)
- 事务、跨设备原子操作
- 二级索引、多机复制
- 批量 API(`PutBatch` / `GetMulti`)
- 运行期变更 N、数据迁移、rebalance
- 跨平台(仅支持 Linux)

### 延后但未排除(未来可能单独立项,不在本路线图)

- backup / restore 工具
- 跨进程访问协议
- cgroup IO 限速集成

---

## 三、锁定的架构决策

下列决策贯穿全部阶段,不再讨论。

### 数据模型

| # | 决策 | 内容 |
|---|---|---|
| D1 | value 大小 | 恒为 `kValueSize = 1 MiB`,不等则拒绝 |
| D2 | value 在设备上 | 数据设备**数据区**只放原始 value 字节,**无 header、无 padding**;P5 起设备头部 8K 为双份超级块例外(设备身份/配对元数据),数据区从偏移 8K 起,逻辑 block 从 0(物理偏移由 IoBackend 加 kDataRegionOffset) |
| D3 | 元数据存放 | value 元数据仅在 RAM(`MetaIndex`)+ WAL(持久化);P5 起设备头部 8K 存放双份设备超级块(引擎 UUID / 设备编号 / 配对关系),不存 value 元数据 |
| D4 | 命名分层 | 设备层用 `BlockId`,数据层用 `ValueMeta` / `kValueSize`;不使用 "chunk" 一词 |

### 寻址与路由

| # | 决策 | 内容 |
|---|---|---|
| D5 | `BlockId` 编码 | `uint64_t`,高 8 位 = `device_id`,低 56 位 = `block_idx`;`logical_byte_offset = block_idx × 1 MiB`,不含设备头部;P5 起数据物理偏移再加 `kDataRegionOffset(8K)` |
| D6 | 路由 hash | xxh3(`util/hash.{h,cpp}`),v2.0 前冻结 |
| D7 | key → 设备组 | `device_group_idx = hash(key) % N`，稳定映射到一组 data / WAL / snapshot 资源 |
| D8 | N 不可变 | N（设备组数量）在 Open 时固定，变更等同 v2.0 |
| D9 | R 不可变 | R（每设备组的 reactor 数）在 Open 时固定；**当前强制 R=1，API 不暴露** |

### 持久化与恢复

| # | 决策 | 内容 |
|---|---|---|
| D10 | value durability | 级别 1/2 的 value 写必须跨过后端持久化边界后才算完成；P5～P8 的 sync / `io_uring` 实现为写完成后调用 `fdatasync`，尚未使用 `RWF_DSYNC` 或设备 FUA。级别 3/4 不强制持久化；SPDK 的具体 FUA / Flush 机制由 P9 详细设计按设备能力裁决 |
| D11 | commit 顺序 | Data（跨过持久化边界）→ WAL（同步持久化）→ Index（P5 实施注：此为级别 1 形态；提交顺序随级别变化，见 doc/P5/README 备忘 #1；所有级别都在写入内存索引后返回） |
| D12 | WAL 拓扑 | 每设备组一份 WAL，无中央 WAL、无跨设备组协调 |
| D13 | WAL 帧头 | `magic:4 \| version:1 \| flags:1 \| entry_type:1 \| reserved:1`,扩展走 entry_type / flags |
| D14 | 数据完整性 CRC | **CRC32C**(`util/crc32`);xxh3 仅用于路由 |
| D15 | snapshot + WAL truncate | **P5 必须含**；snapshot 槽头带 version 字段，snapshot 设备采用 A/B 双槽 |

### 并发模型

| # | 决策 | 内容 |
|---|---|---|
| D16 | 多线程实现 | 无锁;**禁止 `mutex` / `shared_mutex` / 自旋锁** |
| D17 | 公开 API 语义 | sync;内部按需异步对用户透明 |
| D18 | reactor 模型 | 按（设备组，reactor）进行状态分区；reactor 间走 lock-free MPSC queue |
| D19 | 跨设备组通信 | 完全无；每个设备组是独立子系统 |

### 抽象层

| # | 决策 | 内容 |
|---|---|---|
| D20 | `IoBackend` 抽象 | C++20 concept,编译期 dispatch;CMake `CABE_IO_BACKEND` 切换 |
| D21 | `MetaIndex` 抽象 | C++20 concept,与 `IoBackend` 对称;CMake `CABE_META_INDEX` 切换 |
| D22 | 实现演进 | `IoBackend = Sync`(P3) → `IoUring`(P4) → `SpdkIoBackend`(P9);`MetaIndex = Hash`(P3) → 可选 `BPlusTree`(P10)。**P6 修订**:取消构建默认后端、`--backend` 必填;sync 冻结为正确性回归，io_uring 成为 P6~P8 过渡主线和性能锚点。**P9 修订**:SPDK 是长期 I/O 主方向，P9 收敛前保留 sync / io_uring 作回归和差分验证。 |
| D23 | 切换粒度 | 编译期,不支持运行期切换 |

### 其他

| # | 决策 | 内容 |
|---|---|---|
| D24 | 零拷贝 | P8 起 Cabe `ValueBuffer` 为 Put 主零拷贝路径；不满足当前后端的来源、归属、地址、长度或对齐条件时透明复制回退。P9 SPDK 下应用端自备普通内存即使对齐也复制回退，`AllocateValueBuffer(key)` 内部使用 Cabe 自管 DMA 可用内存。 |
| D25 | API 冻结 | P2 一次性冻结公开 API,直到 v2.0 不破坏 |
| D26 | 性能回归红线 | 是否采集、比较和设置门槛由各阶段最新设计决定。P6 是历史性能锚点；P8 与 P9 均可归档原始 bench 数据，但虚拟/loop 设备数据不作性能优劣结论，P9 不设置性能门槛。 |

---

## 四、整体阶段图与版本节奏

```
P0 ──► P1 ──► P2 ──► P3 ──► P4 ──► P4.5 ──► P5 ──► P6 ──► P7 ──► P8 ──► P9 ──► P10 ──► P11 ──► P12
基础   单线   API +  IO +   io_    Free      WAL+   Group   Reactor 零拷    SPDK    B+树    多      可观
设施   程版   fwd-   索引   uring  List      恢复   Commit  无锁MT  贝主路 后端    无锁内存 NVMe    测性
              compat 抽象                    snap.                  径       激活            导出
```

**版本发布策略**：cabe 在全部功能完工（P0–P12）并实际跑通后统一发布 v1.0。开发过程中不设版本里程碑。v2.0 仅在发生 API 不兼容变更时触发（不在本路线图范围）。

---

## 五、各阶段范围

### P0 — 基础设施

**状态**：✅ 已实施（P0M7 收敛通过；详见 [doc/P0/P0M7_convergence_design.md](doc/P0/P0M7_convergence_design.md)）

**目标**:让项目能 build、跑通本地组合矩阵(持续集成在 M6 推迟,待仓库托管确定后单独立项),工具库齐备,数据 schema 与公共约定全部定型;无业务逻辑。

**范围**:
- 项目骨架、根 `CMakeLists.txt`
- CMake 变量预留:`CABE_IO_BACKEND` / `CABE_META_INDEX` / `CABE_SANITIZER`
- 本地 ASAN / TSAN / UBSAN / Release 四档组合矩阵(双工具链)+ 覆盖率脚本;持续集成(CI)推迟,待仓库托管确定
- 测试框架(GTest)与 bench 框架(google-benchmark)接入
- 工具库:
  - `util/crc32.{h,cpp}` — CRC32C(已有)
  - `util/cpu_features.{h,cpp}` — 已有
  - `util/util.h` — 时间戳(已有)
  - **`util/hash.{h,cpp}`** — xxh3 包装(新增,D6)
- **Logger**:`common/logger.h` 接入 stderr 最简实现,**禁止全空操作**
- **错误码段位规划**(`common/error_code.h`):
  - memory: `-100xxx`
  - io: `-101xxx`
  - index: `-102xxx`
  - wal: `-103xxx`
  - engine: `-104xxx`
  - wal_recovery: `-105xxx`
- **核心 schema 定型**(`common/structs.h`):
  - `inline constexpr size_t kValueSize = 1024 * 1024;`
  - `using DeviceId = uint8_t;`
  - `using DataView = std::span<const std::byte>;`
  - `using DataBuffer = std::span<std::byte>;`
  - `struct BlockId { uint64_t raw; ... };`(D5 编码,手动 mask/shift)
  - `enum class ValueState : uint8_t { Active = 0, Deleted = 1 };`
  - `struct ValueMeta { BlockId block; uint64_t timestamp; uint32_t crc; ValueState state; uint8_t reserved[3]; };`（字段重排后恰为 24 字节，8 字节对齐）
  - WAL 帧头 8 字节布局占位常量
- README 含 build 指南、Roadmap 表、依赖列表
- 文档骨架:`doc/P0/P0M7_convergence_design.md`(收敛稿) / `doc/P1/README.md`(占位) / `doc/P2/README.md`(占位)

**里程碑拆分**:P0 工作按 7 个里程碑推进,每个里程碑独立提 PR + review。

- **M1:项目骨架与 CMake 基础**
  - 根 `CMakeLists.txt` + 子目录 CMake(`common/` / `util/` / `test/` / `bench/`)
  - 编译器版本检查(GCC 15+ / Clang 20+)、C++20 标准
  - CMake 选项预留:`CABE_IO_BACKEND` / `CABE_META_INDEX` / `CABE_SANITIZER`
  - 现有 `util/crc32` / `util/cpu_features` / `util/util` 纳入 build
  - 退出条件:`cmake -S . -B build && cmake --build build` 在 GCC 15 与 Clang 20 下均通过

- **M2:`common/structs.h` Schema 定型**
  - `CABE_VALUE_DATA_SIZE` → `kValueSize`(D1)
  - `DataView` / `DataBuffer` 切到 `std::byte`(D4)
  - `BlockId` 改 8/56 编码,手动 mask/shift(D5);提供 `Make` / `dev()` / `block_idx()` / `byte_offset()` 方法(P5M1 注:`byte_offset()` 改名 `logical_byte_offset()`——超级块引入后物理偏移 = kDataRegionOffset + 此值,由 IoBackend 加,见 D8)
  - 新增 `DeviceId = uint8_t`、`enum class ValueState : uint8_t`
  - 旧 `ChunkMeta` / `DataState` 改名删除
  - 新增 `struct ValueMeta`(24 字节对齐),含 `static_assert(sizeof(ValueMeta) == 24)` 与 `static_assert(sizeof(BlockId) == 8)`
  - WAL 帧头 8 字节布局占位常量(`kWalFrameHeaderSize` 等)(P5M2 注:真实 128 字节帧入 `wal/wal_frame.h` 后占位常量已移除)
  - 退出条件:`common/structs.h` 编译通过、无旧名残留;静态断言全部通过

- **M3:错误码段位规划 + Logger stderr 实装**
  - `common/error_code.h`:六段(memory / io / index / wal / engine / wal_recovery),每段 1000 号
  - 现有 memory 段(`-100xxx`)保留
  - `common/logger.h`:stderr 最简实现(~100 行),环境变量 `CABE_LOG_LEVEL`(默认 `WARN`)控制级别
  - 输出格式:`[LEVEL][tid][file:line] message`
  - 退出条件:小 demo 程序能打出五级日志;段位静态不重叠的编译期断言通过

- **M4:`util/hash.{h,cpp}` xxh3 接入**
  - 决策子项:xxhash 系统库 vs 内嵌单头文件(M4 起步时拍板,写入 design 稿)
  - 接口:`uint64_t cabe::util::Hash(DataView)` / `Hash(std::string_view)`
  - 路由辅助:`DeviceId RouteToDevice(std::string_view key, size_t n_devices)`(D7 实装入口)
  - 退出条件:已知向量测试 + 简单分布测试(100K random keys 的 chi-squared)通过

- **M5:测试与 bench 框架接入 + util/common 测试覆盖**
  - GTest + google-benchmark 接入 CMake(优先 `find_package`,fallback `FetchContent`)
  - 测试目录:`test/util/` / `test/common/`;bench 目录:`bench/util/`
  - 覆盖目标:
    - `util/crc32`:已知向量、SSE 与软件 fallback 一致性、空 buffer 边界
    - `util/hash`:已知向量、分布、跨平台稳定性
    - `util/util`:时间戳单调性、wall vs monotonic 语义
    - `util/cpu_features`:冒烟测试
    - `common/structs`:`BlockId` encode/decode 往返、`ValueMeta` 字段对齐、enum 取值
    - `common/error_code`:段位不重叠
    - `common/logger`:级别过滤、格式输出
  - 覆盖率工具(`gcov` / `llvm-cov`)接入,`make coverage` target 生成报告
  - 退出条件:`ctest` 全绿;`util` + `common` 行覆盖率 ≥ 80%

- **M6:本地组合矩阵 + 测试 / 覆盖率脚本**(详见 [doc/P0/P0M6_test_scripts_design.md](doc/P0/P0M6_test_scripts_design.md))
  - CMake `CABE_SANITIZER` 选项实装(`address` / `thread` / `undefined` / `none`) —— **已在 M1 提前完成(M1-D1 偏差)**
  - `scripts/run-tests.sh`:单次调用 `--asan` / `--tsan` / `--ubsan` / `--release` + `--filter` / `--clean` / `--jobs` / `--backend=`(P4 预留);本地四档独立跑通
  - `scripts/run-coverage.sh`:`util` / `common` 行覆盖率报告(`gcovr` / `llvm-cov`),`--strict` 硬卡 ≥ 80%
  - `scripts/setup-dev.sh`:`REQUIRED_PKGS` 补 `gcovr`
  - **持续集成(CI)工作流推迟**:cabe 当前为实验性 demo、仓库托管未定;待托管确定后单独立项(不属 P0 路线图剩余里程碑)
  - 退出条件:本地八格 `ctest` 全绿 + `run-coverage.sh` 通过(`util/*.cpp` 行覆盖率 ≥ 80%)

- **M7：P0 设计稿固化与状态同步**
  - `doc/P0/P0M7_convergence_design.md` 完整撰写（薄索引形态——每章摘要 + 链回 P0M1–M6 对应章节；schema / 错误码段位 / 术语表 / CMake 选项 / 本地组合矩阵与覆盖率约定（CI 不涵盖）/ 测试·微基准约定）
  - `doc/P1/README.md` / `doc/P2/README.md` 阶段占位索引（含 ROADMAP 范围摘要 + 已知决策点候选 + 启动条件）
  - `scripts/run-bench.sh` 实装；P0 当时归档 `bench/baselines/p0_utilities.json`（P6 建立正式锚点后已删除）
  - 评审残留 #9–#15 共 7 项 LOW 防御性问题全部清场（P0 收敛点零债务）
  - 各 P0M1–M6 设计稿状态字串 → "✅ 已锁定（P0M7 收敛）"
  - `ROADMAP.md` P0 状态字串 → "已实施"；根 `README.md` 表格 P0 → "✅ 完成"
  - `doc/P0/README.md` 退出条件 6 条 + M7 条目按 M6 / M7 决策同步
  - 退出条件：见 [doc/P0/P0M7_convergence_design.md](doc/P0/P0M7_convergence_design.md) §10

**里程碑依赖与并行度**:

```
       ┌──► M2 ──┐
M1 ──► ├──► M3 ──┤──► M5 ──► M6 ──► M7
       └──► M4 ──┘
```

M2 / M3 / M4 不互相依赖,可并行;实际建议按 M2 → M3 → M4 串行提 PR 便于 review(每 PR 集中一个主题)。M7 可与 M5/M6 部分并行起草,最终在 M6 完成后定稿。

**P0 退出条件**:
1. GCC 15+ 与 Clang 20+ 双工具链 build 通过
2. ASAN / TSAN / UBSAN / Release 四档 × 双工具链本地 `ctest` 全绿(持续集成 CI 推迟到 P0 之外单独立项)
3. 工具库(util / common)单测行覆盖 ≥ 80%(`scripts/run-coverage.sh --strict` 实证)
4. `doc/P0/P0M7_convergence_design.md` 审阅通过,锁定本阶段所有 schema 决策
5. README 与本文件同步
6. P0 当时完成 `bench/baselines/p0_utilities.json` 归档；P6 起已删除、不再作为参考

---

### P1 — 单线程版核心引擎

**状态**：✅ 已实施（P1M5 收敛通过）

**目标**:跑通完整 Put / Get / Delete 路径。**单线程、无持久化、纯 RAM 索引**。

**范围**:
- `cabe::Options` / `cabe::Status` 公开类型(P2 才冻结)
- `cabe::Engine` 公开类骨架:`Open` / `Put` / `Get` / `Delete` / `Close`
- **内部按 per-device 形态**:`Engine` 持有 `std::vector<DeviceContext>`,P1 内 `size() == 1`
- `struct DeviceContext { IoBackend io; FreeList free; MetaIndex index; }` 雏形
- key 路由函数 `size_t Engine::RouteKey(string_view)`,P1 内永远返回 0
- 朴素 I/O(syscall + O_DIRECT,不抽象)
- 朴素 BufferPool(对齐到 4 KiB 的 1 MiB 块池)
- 朴素 FreeList(`std::vector<BlockId>` + LIFO)
- 单层 MetaIndex(直接 `std::unordered_map<std::string, ValueMeta>`,**不引入抽象层**)
- 单 Put / Get / Delete 完整路径
- 严格 `value.size() == kValueSize` 校验
- 单元测试 + 微基准曾归档到 `bench/baselines/p1_single_thread.json`；P6 起已删除、不再作为参考

---

### P2 — 公开 API 冻结声明

**状态**：✅ 已实施（P2M2 收敛通过）

**目标**:审查 P1 已实装的公开接口,确认能撑到项目完工,声明冻结意图(后续尽量不改;如果被迫要改,同步更新文档)。

**范围**:
- 审查 P1 公开 API:`Engine::Open / Put / Get / Delete / Close` 签名、`Options` / `Status` 类型
- `Options` 形态审查:`DeviceConfig` 是否够用、是否需加 reserved 字段(P5 起已扩为三路径 data/wal/snapshot + create / WAL / 快照 / 恢复字段)
- `Status` 错误码空间评估:P2 以六段 × 1000 起步;后续保持旧码值不变并按需追加新段(P5 snapshot、P9 SPDK 专属段)
- Engine 承诺语义审查:析构自动 Close、Put 部分写、Open 幂等
- 输出**公开 API 符号清单 + 冻结声明**文档
- **不实现并发安全**;P2–P6 单线程访问,多线程语义在 P7(P6M1 边界精确化:此约束指**公开 API**;`Wal` 模块内部自 P6M1 起多写者 `WriteWal` 并发安全——group commit 机制先行,由多线程并发测试驱动直打验证,P7 即插即用,见 doc/P6/README.md D1)
- **不做前向兼容概念验证**:reactor / 多 device / 零拷贝 / recovery 的验证推迟到各自功能实装后

**退出条件**:公开 API 符号清单审阅通过 + 冻结声明文档输出。冻结为设计意图声明(尽量保证),非绝对约束——全部完工发布后才是严格约束。

---

### P3 — IoBackend 与 MetaIndex 抽象层

**状态**：✅ 已实施（P3M4 收敛通过；详见 [doc/P3/P3M4_convergence_design.md](doc/P3/P3M4_convergence_design.md)）

**目标**:把"I/O 路径"和"索引数据结构"两个可替换组件同时抽象化,为后续多后端 / 多索引实现做准备。

**范围**:
- **`IoBackend` C++20 concept**:
  - 绑定到一个 device
  - P3 初始同步签名为 `Write(block_idx, const byte*)` / `Read(block_idx, byte*)`;P8M4 已增加 `RegisterWriteBuffers(span<ValueBufferSlotView>)`,并把现行写接口升级为 `Write(block_idx, const IoWriteBuffer&)`;读接口保持不变
  - `SyncIoBackend` 完整实现(O_DIRECT + pread / pwrite,包装 P1 已有的 WriteBlock / ReadBlock)
  - P4 `io_uring` 使用提交即等待；P9 SPDK 首轮使用命令提交 + completion 轮询；二者对 Engine 均保持同步，深度异步归性能兑现阶段
- **`MetaIndex` C++20 concept**:
  - P3 初始接口包含 `WriteSnapshot` / `LoadSnapshot`;P5M4 已将 snapshot I/O 上移并收窄为:`Insert` / `Lookup` / `Delete` / `Size` / `Contains` / `ForEach`
  - `HashMetaIndex` 实现包装 `unordered_map`;`ForEach` 已实装并供结构无关 snapshot 模块遍历
  - 契约测试套件(任何实现都要通过)
- CMake:`CABE_IO_BACKEND` 与 `CABE_META_INDEX` 变量编译期分派生效
- Engine 切换到两层抽象,功能等价于 P2
- **不做**:ValueBuffer(P8) / 伪 SPDK(P9) / 异步接口(P4)

---

### P4 — io_uring 后端

**目标**:io_uring 后端,启用 registered buffers + FIXED ops + register_files。

**范围**:
- `IoUringIoBackend` 完整实现
- liburing ≥ 2.9 接入
- 每 `(device, reactor)` 一个独立 ring
- registered buffer 注册、IOSQE_FIXED_FILE
- submit / wait 模型(P7 才真正多线程)
- TSAN 与 io_uring 双层阻断处理
- 部署文档:ulimit / RLIMIT_MEMLOCK / sysctl `kernel.io_uring_disabled`
- bench 标注"基于朴素 FreeList + 单 device + 单线程"

---

### P4.5 — 块分配器改造

**状态**：✅ 已实施（P4.5M3 收敛通过；详见 [doc/P4.5/P4.5M3_convergence_design.md](doc/P4.5/P4.5M3_convergence_design.md)）

**目标**:朴素 FreeList（LIFO 栈）→ 可插拔块分配器抽象层（C++20 concept）+ 固定大小环形队列（FIFO）默认实现。

**范围**:
- `BlockAllocator` C++20 接口定义（`slots/block_allocator.h`）,CMake `CABE_BLOCK_ALLOCATOR` 编译期切换
- `RingBlockAllocator` 默认实现（`slots/ring/ring_block_allocator.*`）:固定大小环形队列,FIFO 语义,容量 = block_count + 1
- 方法:`Init` / `Acquire` / `Recycle` / `Available` / `Empty` / `RebuildFromActive`
- Engine 切换到块分配器抽象层,DeviceContext 持有 `BlockAllocatorImpl`
- TRIM 占位:Engine::Delete 路径预留调用点;P7 只把调用点迁入 reactor,未实现实际 discard,继续归性能兑现阶段
- `RebuildFromActive(span<BlockId>)` 接口,为 P5 崩溃恢复服务
- 删除旧 `engine/free_list.*`
- **不做**:三容器轮换(升序分配对 1M 块无性能价值)/ 异步 TRIM(性能兑现阶段)/ 独立性能基准

---

### P5 — 持久化与崩溃恢复

**状态**：✅ 已实施（P5M7 收敛通过；详见 [doc/P5/P5M7_convergence_design.md](doc/P5/P5M7_convergence_design.md)）

**目标**:让 cabe 从纯内存索引走向真正持久化——设备超级块、per-device WAL、快照 + 环形队列、崩溃恢复。启动时从持久化状态重建内存索引。详细里程碑划分与决策见 [doc/P5/README.md](doc/P5/README.md)。

**范围**:
- **设备超级块**:每个数据设备关联 WAL 设备 + 快照设备,三块设备通过超级块(引擎全局 UUID + 设备编号 + 配对 UUID + 自身 CRC32C)关联校验;防盘符漂移。bcache 风格:三设备头部 8K 双份超级块,数据区从偏移 8K 起,逻辑 block 从 0(物理偏移由 IoBackend 加,D8)
- **启动模式**:仅 create(新建)/ recover(恢复)两种——内存索引易失,每次启动必须重建
- **per-device WAL**:每个数据设备独立 WAL,存放在独立 WAL 裸设备上,仅记录元数据
- **WAL 4 级持久化**(Options 全局配置,默认级别 3):
  - 级别 1:value 落盘 + WAL 落盘 + 内存写入再返回(最严格)
  - 级别 2:value 落盘 + 内存写入再返回(WAL 异步攒批)
  - 级别 3:WAL 落盘 + 内存写入再返回(value 异步)——默认
  - 级别 4:仅内存写入再返回(全异步)
  - 不变量:所有级别先写内存索引再返回(读己之写)
- **WAL 帧格式**:128 字节固定帧(4096 的因数,32 帧填满一个 4K 块,不跨块);含单调序号 seq(定序 + 恢复定边界)、帧自身 CRC32C(校验完整性)+ value 的 CRC32C(读时校验);entry_type 区分 Put / Delete
- **快照 + 环形队列**:MetaIndex 全量镜像写入独立快照设备;WAL 设备以环形队列管理,快照后截断回收(P5M5 留 TRIM 空桩;P7 未实现,继续归性能兑现阶段);触发策略 = 大小阈值(主)+ 手动 `Engine::Snapshot()`(辅)
- **崩溃恢复** per-device 并行:超级块校验 → 加载快照 → 重放 WAL(CRC 校验定边界)→ `BlockAllocator::RebuildFromActive`(位图反推)(P5M6 注:"并行"为多设备远景(P7/P11);当前 N=1,P5M6 实装为单设备串行恢复链)
- **WAL / 快照 I/O**:P5 Raw 路径使用同步 `RawDevice`;P7 通过 reactor 串行拥有这些组件但没有改写其 I/O;P9M8~P9M11 将分别引入专用设备抽象并适配 SPDK
- 测试:基础恢复测试 + WAL 损坏测试(完整崩溃注入矩阵推迟)
- **不做**:指标接口(P12)/ WAL 深度异步化(性能兑现阶段)/ Group Commit(P6)/ 独立性能基准

---

### P6 — Group Commit

**状态**:✅ 已实施(P6M3 收敛通过即收尾;名义挂靠的 M4 锚点数据也已人工采集并归档,详见 doc/P6/README.md)。

**目标**:WAL fsync 合并,提升并发写吞吐。

**范围**:
- WAL writer 改造为"积攒帧 + 共享 flush"
- leader / follower 协议或 batching window(设计稿二选一,推荐 leader/follower)
  (P6 定案:**leader/follower**——单线程退化零开销、无后台线程、负载自适应;batching
  window 因人为延迟对单线程是纯损失被否决,见 doc/P6/README.md D2)
- 在途帧的并发协调(lock-free MPSC queue)(P6M1 落地形态:无锁 MPSC 提交**栈**
  ——CAS push + leader 一次 exchange 全摘、反转复到达序,对外语义等价队列;全摘设计
  使 ABA 与安全回收问题结构性免除,见 P6M1 稿 §3)
- tail latency 控制(batch size + 最长等待时间上限)(P6M1 裁决:**两者皆不引入配置项**
  ——leader/follower 无窗口等待;批 ≤ 并发写者数(`WriteWal` 阻塞语义)、follower
  等待 ≤ 两个批处理时长,上界由协议结构天然给出,见 P6M1 稿 §5.4)
- 失败语义:fsync 失败整 batch 同步返回失败

**性能基线**:P6 是 cabe 的性能**锚点**——P6 完工时采集**单线程 12 条矩阵**(`Put`/`Get`/`Delete`
× WAL 级别 1/2/3/4)+ **多线程 `Wal` 提交吞吐**(1/2/4/8 写者),度量 = 吞吐(IOPS@1MiB)+ 自带延迟;
后续 P7 / P8 已按各自阶段设计采集原始数据；由于使用 loop 设备，只作路径样本、不作性能优劣结论。**P9-D22 覆盖这里早期预写的 P9 对比承诺**：P9 只采集并归档
SPDK bench 原始 JSON，不与前一阶段作性能优劣比较、不设性能门槛。P6 为首个锚点,自身不设回归门槛——单线程写不因 group commit 退化,由 P6M1 设计的
结构性论证(见 P6M1 稿 §5.5/§6.2)保证。

**后端策略(P6 定案,P9 修订)**:sync 是早期为快速落地走的同步路线;P7 在 io_uring 过渡后端上完成
reactor / 无锁 / 多线程框架迁移,但仍采用提交即等待,没有实现 io_uring 真异步。据此截至 P6:① 冻结 sync 的**开发/优化 + 性能基准**,
**保留 sync 正确性回归测试**(作 io_uring 差分参照,且 TSAN 仅 sync 可跑——io_uring 不兼容,
数据竞争证据只能由 sync+TSAN 产出);② **性能锚点 = io_uring**(同步用法、单线程引擎;提交组
后端无关,故 io_uring 锚点测的就是 P6 真做的提交组),上文「性能基线」的锚点即在 io_uring 后端
采集;③ **取消构建默认后端、`--backend` 必填**(CMake + 三脚本已实装)。细化见 doc/P6/README.md
D10 与 doc/P6/P6M3 §6.5(P6M3-D14~D17)。

---

### P7 — Reactor 并发模型 + 无锁多线程 + 多 device 端到端

**状态**：✅ 已实施（P7M5 收敛通过；五里程碑 M1~M5 全部实装，7 配置全量回归全绿，详见 [doc/P7/P7M5_isolation_convergence_design.md](doc/P7/P7M5_isolation_convergence_design.md)）

**目标**:per-reactor 状态分区 + 消息传递,**无任何 mutex**;公开 API 自此冻结;多 device 端到端跑通。

**范围**:
- 每 device 一个 reactor(R=1),独占 `DeviceContext`;`BlockAllocator` / `BufferPool` 因 reactor 单线程所有权保持普通非原子实现
- 设备级 hash 路由落地:`device = hash(key) % N`;R>1 的第二级路由只保留模型,未写入代码
- 调用线程通过 lock-free MPSC 入站队列投递栈驻 op,使用 C++20 `atomic::wait/notify` 同步等待
- `Put` / `Get` / `Delete` / `SetWalLevel` / `Snapshot` / `Close` 统一经 reactor 串行执行
- P7 保持 io_uring 提交即等待;流水线、多请求在飞、SQPOLL、DEFER_TASKRUN、TRIM、R>1 和钉核均推迟到性能兑现阶段

**Milestone 拆分**:
- **M1**:reactor 骨架、读路径与同步包异步
- **M2**:写路径与运营操作迁入 reactor
- **M3**:单设备多线程正确性
- **M4**:多设备路由、恢复与运营操作 fan-out
- **M5**:隔离边界与阶段收敛

**性能观察项**:单线程 p50 与多设备 QPS 原红线在 P7 最终裁决为观察项,不作退出门槛;loop 设备不用于真实性能结论,真盘规模度量归 P11。

---

### P8 — 零拷贝写入路径(主路径化)

**状态**:✅ 已实施(P8M1~P8M5 全部完成;bench 原始 JSON 已人工归档,loop 设备结果不作性能优劣结论)。

**目标**:零拷贝成为默认 Put 路径;不对齐 buffer 隐性 fallback。

**范围**:
- 用户 buffer 通过 cabe 提供的 allocator 分配(从 per-device registered buffer pool 取)
- 对齐要求:1 MiB strict
- 不满足当前后端的来源、归属、地址、长度或对齐条件时自动走 copy 路径,API 不分裂、不报错
- io_uring registered buffer 协议升级
- bench 覆盖非对齐自备内存、对齐自备内存和 Cabe `ValueBuffer`;只归档原始数据,不作性能优劣结论
- 文档:使用约束 + cabe allocator 用法

---

### P9 — SPDK NVMe API 后端

**状态**：🚧 P9M0/P9M1 已实现并通过构建、配置与回归验证，下一步进行 P9M2 详细设计；P9M1-D19（SPDK sanitizer/覆盖率构建变体）仍待后续裁决，详见 [doc/P9/README.md](doc/P9/README.md)

**目标**:直接基于 SPDK NVMe API 接入 NVMe 设备,让 value/data、WAL、snapshot 和超级块读写逐步迁移到 SPDK 后端。P9 不走 SPDK bdev 路线,也不把 SPDK 当成外部服务。

**范围**:
- 将 SPDK 固定为 `third_party/spdk` 子模块,由 Cabe 脚本管理初始化、依赖、编译、检查、大页内存和显式设备接管。
- 新增类型化 SPDK 设备配置,使用显式 `BDF + namespace id + 可选字节区间` 表达 data、WAL、snapshot namespace 设备视图。
- 新增 Cabe 自带 SPDK 验证工具,先完成定向 probe、namespace 枚举、DMA 分配、qpair 创建和 1MiB 读写校验。
- 引入内部 `SpdkNvmeDevice` 薄封装和 `SpdkIoBackend`,先让 value/data 路径可工作,再接入 P8 `ValueBufferPool` 形成 SPDK 零拷贝路径。
- 将 WAL、snapshot 和超级块读写从 `RawDevice` 逐步迁移到可替换设备抽象,分别补 SPDK 适配。
- 完成全 SPDK 模式 create、recover、Put、Get、Delete、Snapshot、Close 的端到端验证。
- 归档 SPDK bench 原始数据,但 VMware 虚拟 NVMe 数据只作为路径样本,不作为真实性能结论。

**显式不做**:
- SPDK bdev、外部 SPDK 服务或跨进程 SPDK 访问协议。
- 应用端直接管理或导入 SPDK DMA 内存。
- 多请求在飞、poll group、qpair 队列深度调优和真实硬件性能结论。
- B+树索引实现。

---

### P10 — 生产级无锁内存 B+树索引

**目标**:在 `MetaIndex` 抽象层下实现高性能、生产级、内存型、无锁的
`BPlusTreeMetaIndex`,既作为核心技术学习路径,也作为 Cabe 可选索引实现。`HashMetaIndex`
继续保留,不会仅因 P10 完成就自动切换默认实现。

**当前设计边界**:P9 只锁定阶段顺序和下列边界；P10 的算法选择、节点布局、内存回收方案、
测试证明方法和里程碑数量必须在 P10 正式设计讨论中逐项裁决,不沿用早期“四个里程碑”预测。

- 完整满足现行 `MetaIndexBackend` 的 `Insert` / `Lookup` / `Delete` / `Size` / `Contains` /
  `ForEach` 契约。
- B+树只管理内存索引,不拥有裸设备 I/O,不自行定义持久化页格式。
- snapshot 继续由 P5 的结构无关 `snapshot` 模块负责,通过 `ForEach` 导出、`Insert` 恢复；
  不恢复已经从 concept 删除的 `WriteSnapshot` / `LoadSnapshot`。
- 索引模块本身以无锁并发实现为目标,禁止 `mutex` / `shared_mutex` / 自旋锁；线性化点、节点
  分裂合并和安全内存回收必须有可审查的正确性说明。Engine 当前仍可保持 per-reactor 单线程
  所有权,不为展示索引并发能力而破坏 P7 架构。
- 实施按学习曲线切成足够小的可运行里程碑,先证明单线程树结构与增删查正确,再引入并发更新、
  内存回收、Engine 接入和收敛验证。
- 测试至少包含参考容器差分、随机增删查、结构不变量、并发线性化验证、检测器矩阵和长时间压力；
  bench 数据用于记录和理解行为,是否比较或切换默认实现由 P10 最新设计另行决定。

**显式不做**:
- 盘上 B+树或由 B+树接管 WAL / snapshot；
- 向公开 KV API 增加范围扫描；
- 在 P10 正式设计前锁死节点页大小、扇出、并发算法或内存回收技术；
- 根据 loop 或虚拟设备数据预先承诺性能倍数。

---

### P11 — 多 NVMe 规模化与隔离验证

**目标**:验证多 device 大规模部署可用(N ≥ 8);架构改造在 P7 已完成,本阶段聚焦真盘规模化、隔离和性能验证。

**范围**:
1. `N ∈ {2, 4, 8}` 配置矩阵端到端测试
2. key 分布均衡性测量(xxh3 在真实 key 分布下的负载偏斜)
3. 多 device 聚合带宽 bench(单盘极限 → N 盘聚合的线性度)
4. 单 device 故障隔离深度测试(写失败、读失败、整盘掉线)
5. 与真盘验证直接相关的部署记录:设备列表配置、故障处置、N 不可变的明确告知；通用运维工具和完整手册归 P12

**显式不做**:
- 跨 device 事务 / 原子 Put
- 热添加 / 热移除 device
- key 在 device 间的迁移 / rebalance

---

### P12 — 可观测性导出与运维工具

**目标**:Metrics 接入与导出(生产格式);运维工具链补齐。
(P5 对账注:原文"把 P5 已经接入的 Metrics 接口导出"已失效——P5-D6 把指标接口整体推迟、P5 未接入任何 Metrics;P12 需先补接入再导出,工作量按此评估。)

**范围**:
- Metrics 导出(Prometheus / OpenMetrics),per-device label + 全局聚合
- 慢操作日志(可配置阈值)
- 健康检查 API(`Engine::HealthCheck()`,per-device 状态)
- 命令行工具:
  - `cabe-info`:查看 device 配置、容量使用、reactor 数量
  - `cabe-fsck`:离线一致性检查(WAL ↔ MetaIndex ↔ BlockAllocator)
  - `cabe-dump`:dump key 列表或某 key 的 `ValueMeta`
- 运维手册 + 压测方法论文档

---

## 六、阶段间衔接约定

每阶段完成需提交:

1. 代码合入主分支,**四档检测器 CI 全绿**(P4+ TSAN 与 io_uring 组合除外)
2. 阶段设计稿 `doc/pN_xxx_design.md` 更新为"已实施",含取舍记录
3. 按阶段最新设计决定是否归档 bench；需要归档时保存到 `bench/baselines/`
4. 仅在阶段设计明确定义门槛时执行**性能回归红线检查**
5. README Roadmap 表对应阶段标记完成
6. 下一阶段设计稿启动(空文件 + 范围草稿)

> **P5 收敛对账注**（P5M7）：#3 与 #6 对 P5 豁免——#3 按 P5 范围"性能基准发版后补"
> （且 M5/M6 两度变更 bench 语义，历史数字可比性已断）；#6 按 P5-D1"收敛稿不引入
> 下一阶段占位"（更具体更晚近的决议），P6 文档随 P6 启动时自建。其余四条照常履行
> （#1 以本地四档矩阵代行——CI 推迟为 P0M6 既定）。详见 P5M7 收敛稿 §11。

> **性能基线策略注**（P6 起）：#3（bench 归档）/#4（性能回归红线）**非逐阶段强制**——
> P6 建立历史性能锚点；P7 / P8 已归档 loop 原始数据但不作性能优劣结论。**P9-D22 覆盖早期“P9 相对前一基线
> 对比”的承诺**：P9 仅保存 SPDK bench 原始 JSON，不作性能优劣结论、不设置性能门槛；VMware
> 虚拟 NVMe 数据只作为路径样本。
> **锚点采集在 io_uring 后端**（同步用法、单线程；sync 自 P6 冻结性能基准、仅保留正确性
> 回归——见 P6 段「后端策略」与 doc/P6/README.md D10）。
> P0/P1 早期归档的基线（`p0_utilities.json` / `p1_single_thread.json`，设计思路未定型时所做）
> **已删除、不再作为参考**——一律以 P6 基线为准。bench 工具代码（`run-bench.sh` / 各
> `*_bench.cpp`）保留，供 P6 建基线复用。详见本路线图 P6 段「性能基线」段落、
> doc/P6/README.md（P6M1-D28）与 doc/P6/P6M2 §3。

---

## 七、关键约束(贯穿全期)

- Linux only,Fedora 43+ / 内核 6.16+
- C++20,GCC 15+ 或 Clang 20+
- TSAN 支持(P4+ io_uring 组合除外)
- **后端路线**:P6 起 sync 冻结开发/优化/性能基准(仅留正确性回归),P6~P8 的过渡主线与性能锚点为 io_uring；P9 直接接入 SPDK NVMe API，并将 SPDK 作为长期 I/O 主方向。构建无默认后端,`--backend` 必填；P9 收敛前保留 sync / io_uring 作回归和差分验证。
- 裸设备语义,不创建 / 不 truncate / 不 unlink 设备节点
- **假定软硬件不发生运行时故障**(cabe 为学习 demo、非工业品):① 明显故障(设备打开 / 内存分配 / 参数非法)必须有简单判断、保证健壮;② 运行时系统 / 硬件异常(`WriteAt`/`Sync` 等 I/O 故障)一概不投入——防御性返回兜底但不做故障注入测试、完整性仅靠代码审查、代码以"运行时故障未测"标出;③ `kWalFull` 等资源状态不是故障、照常真测。不为内核 bug / 设备掉线做应用层兜底。详见 doc/P6/P6M2_concurrency_audit_design.md §3
- **任何阶段不得触发公开 API 破坏**;评估为必须则升级为 v2.0 候选,独立立项
- **N（设备组数量）在 Open 时固定**，运行期不可变；变更等同 v2.0

---

## 八、术语表

| 术语 | 层 | 定义 |
|---|---|---|
| **value** | 数据层 | 用户传入 / 取出的字节负载,大小恒为 `kValueSize`(1 MiB) |
| **`kValueSize`** | 常量 | 1 MiB = 1048576 字节 |
| **`BlockId`** | 设备层 | 逻辑寻址:`device_id:8 \| block_idx:56`;逻辑字节偏移 = `block_idx × 1 MiB`,物理数据偏移由后端再加头部 8K |
| **设备组** | 配置 / 运行时 | 一组 data、WAL、snapshot 设备资源；当前由一个 reactor 独占一个 `DeviceContext`，P9 中各角色由显式 `BDF + namespace id + 可选字节区间` 配置 |
| **data / WAL / snapshot 设备** | 设备层 | 设备组内三种物理角色；P5～P8 为裸块设备路径，P9 SPDK 路径对应显式 NVMe namespace |
| **block** | 设备层 | data 设备上的一个 1 MiB 物理数据区域；**1 block 存 1 value** |
| **`ValueMeta`** | 数据层 | 内存索引中关于一个已存 value 的元数据 `{BlockId, timestamp, crc, state, reserved}` |
| **`MetaIndex`** | 数据层 | `key → ValueMeta` 的索引,abstraction(D21) |
| **WAL** | 持久化层 | 写前日志,per-device,所有元数据变更的真相源 |
| **snapshot** | 持久化层 | MetaIndex 的周期性磁盘镜像,用于 WAL truncate 与加速 recovery |
| **N** | 配置 | 设备组数量，Open 时固定 |
| **R** | 配置 | 每设备组的 reactor 数，Open 时固定，当前 = 1 |
| **reactor** | 并发层 | 独占一个设备组的 `DeviceContext`（data I/O、BlockAllocator、MetaIndex、WAL、snapshot）的执行单元 |

---

## 九、决策汇总表(附录)

完整内容见第三节(D1–D26)。此处列出每个决策的阶段绑定:

| 决策 | 简述 | 锁定阶段 |
|---|---|---|
| D1 | value 严格 1 MiB | P0 |
| D2 | 数据区只存原始 value；设备头部 8K 为双份超级块 | P0/P5 |
| D3 | value 元数据在 RAM + WAL；设备身份元数据在超级块 | P0/P5 |
| D4 | 命名分层(BlockId / ValueMeta) | P0 |
| D5 | BlockId 8/56 编码 | P0 |
| D6 | xxh3 路由 | P0 |
| D7 | hash(key) % N 路由 | P2 |
| D8 | N 不可变 | P2 |
| D9 | R 不可变,初期 R=1 | P2 |
| D10 | value 持久性随 WAL 四级策略变化 | P5 |
| D11 | 提交/返回顺序随 WAL 四级策略变化 | P5 |
| D12 | WAL per-device | P5 |
| D13 | WAL 帧头 8 字节 | P5 |
| D14 | CRC32C 数据完整性 | P5 |
| D15 | Snapshot + WAL truncate | P5 |
| D16 | 无锁多线程 | P7 |
| D17 | sync API + 内部异步 | P2 |
| D18 | per-reactor 状态分区 | P7 |
| D19 | 跨 device 无通信 | P7 |
| D20 | IoBackend concept | P3 |
| D21 | MetaIndex concept | P3 |
| D22 | 后端与索引实现演进；I/O 后端无构建默认值 | P3/P6/P9 |
| D23 | 编译期切换 | P3 |
| D24 | 零拷贝主路径 | P8 |
| D25 | API 冻结于 P2 | P2 |
| D26 | 性能采集、比较与门槛由各阶段最新设计决定 | 各阶段（见六节策略注） |
