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
- 部署:Linux 用户态进程,直接打开块设备节点
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
| D2 | value 在设备上 | 数据设备只放原始 value 字节,**无 header、无 padding** |
| D3 | 元数据存放 | 仅在 RAM(`MetaIndex`)+ WAL(持久化),数据设备不存任何元数据 |
| D4 | 命名分层 | 设备层用 `BlockId`,数据层用 `ValueMeta` / `kValueSize`;不使用 "chunk" 一词 |

### 寻址与路由

| # | 决策 | 内容 |
|---|---|---|
| D5 | `BlockId` 编码 | `uint64_t`,高 8 位 = `device_id`,低 56 位 = `block_idx`;字节偏移 = `block_idx << 20` |
| D6 | 路由 hash | xxh3(`util/hash.{h,cpp}`),v2.0 前冻结 |
| D7 | key → device | `device_idx = hash(key) % N`,稳定函数关系 |
| D8 | N 不可变 | N(设备数)在 Open 时固定,变更等同 v2.0 |
| D9 | R 不可变 | R(每 device 的 reactor 数)在 Open 时固定;**初期强制 R=1,API 不暴露** |

### 持久化与恢复

| # | 决策 | 内容 |
|---|---|---|
| D10 | value durability | FUA(`RWF_DSYNC` 或 io_uring 对应 flag);value 路径无 fsync |
| D11 | commit 顺序 | Data(FUA) → WAL(fsync) → Index |
| D12 | WAL 拓扑 | per-device,无中央 WAL,无跨 device 协调 |
| D13 | WAL 帧头 | `magic:4 \| version:1 \| flags:1 \| entry_type:1 \| reserved:1`,扩展走 entry_type / flags |
| D14 | 数据完整性 CRC | **CRC32C**(`util/crc32`);xxh3 仅用于路由 |
| D15 | snapshot + WAL truncate | **P5 必须含**;snapshot 文件头带 version 字段 |

### 并发模型

| # | 决策 | 内容 |
|---|---|---|
| D16 | 多线程实现 | 无锁;**禁止 `mutex` / `shared_mutex` / 自旋锁** |
| D17 | 公开 API 语义 | sync;内部按需异步对用户透明 |
| D18 | reactor 模型 | per-(device, reactor) 状态分区;reactor 间走 lock-free MPSC queue |
| D19 | 跨 device 通信 | 完全无;每 device 独立子系统 |

### 抽象层

| # | 决策 | 内容 |
|---|---|---|
| D20 | `IoBackend` 抽象 | C++20 concept,编译期 dispatch;CMake `CABE_IO_BACKEND` 切换 |
| D21 | `MetaIndex` 抽象 | C++20 concept,与 `IoBackend` 对称;CMake `CABE_META_INDEX` 切换 |
| D22 | 默认实现 | `IoBackend = Sync`(P3) → `IoUring`(P4) → `Spdk`(P10);`MetaIndex = Hash`(P3) → 可选 `BPlusTree`(P9) |
| D23 | 切换粒度 | 编译期,不支持运行期切换 |

### 其他

| # | 决策 | 内容 |
|---|---|---|
| D24 | 零拷贝 | P8 起为默认 Put 路径;非对齐 buffer 隐性 fallback 到 copy |
| D25 | API 冻结 | P2 一次性冻结公开 API,直到 v2.0 不破坏 |
| D26 | 性能回归红线 | 各阶段衔接含明文红线,超出需 review 通过 |

---

## 四、整体阶段图与版本节奏

```
P0 ──► P1 ──► P2 ──► P3 ──► P4 ──► P4.5 ──► P5 ──► P6 ──► P7 ──► P8 ──► P9 ──► P10 ──► P11 ──► P12
基础   单线   API +  IO +   io_    Free      WAL+   Group   Reactor 零拷    B+树    SPDK    多      可观
设施   程版   fwd-   索引   uring  List      Metric Commit  无锁MT  贝主路 学习路径 后端    NVMe    测性
              compat 抽象                    snap.                  径              激活    导出
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
- **Logger**:`common/logger.h` 接入 stderr 最简实现,**禁止全 no-op**
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
  - `struct ValueMeta { BlockId block; uint32_t crc; uint64_t timestamp; ValueState state; };`(24 字节对齐)
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
  - `BlockId` 改 8/56 编码,手动 mask/shift(D5);提供 `Make` / `dev()` / `block_idx()` / `byte_offset()` 方法
  - 新增 `DeviceId = uint8_t`、`enum class ValueState : uint8_t`
  - 旧 `ChunkMeta` / `DataState` 改名删除
  - 新增 `struct ValueMeta`(24 字节对齐),含 `static_assert(sizeof(ValueMeta) == 24)` 与 `static_assert(sizeof(BlockId) == 8)`
  - WAL 帧头 8 字节布局占位常量(`kWalFrameHeaderSize` 等)
  - 退出条件:`common/structs.h` 编译通过、无旧名残留;静态断言全部通过

- **M3:错误码段位规划 + Logger stderr 实装**
  - `common/error_code.h`:六段(memory / io / index / wal / engine / wal_recovery),每段 1000 号
  - 现有 memory 段(`-100xxx`)保留
  - `common/logger.h`:stderr 最简实现(~100 行),环境变量 `CABE_LOG_LEVEL`(默认 `WARN`)控制级别
  - 输出格式:`[LEVEL][tid][file:line] message`
  - 退出条件:小 demo 程序能打出五级日志;段位静态不重叠的编译期断言通过

- **M4:`util/hash.{h,cpp}` xxh3 接入**
  - 决策子项:xxhash 系统库 vs 内嵌 single-header(M4 起步时拍板,写入 design 稿)
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
    - `util/cpu_features`:smoke test
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
  - `scripts/run-bench.sh` 实装；工具库微基准基线归档到 `bench/baselines/p0_utilities.json`（crc32 / hash 双工具链 Release × 5 次重复 × 中位数）
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
6. `bench/baselines/p0_utilities.json` 归档(crc32 / hash 双工具链 Release 中位数)

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
- 单元测试 + 微基准 baseline 归档到 `bench/baselines/p1_single_thread.json`

---

### P2 — 公开 API 冻结声明

**状态**：✅ 已实施（P2M2 收敛通过）

**目标**:审查 P1 已实装的公开接口,确认能撑到项目完工,声明冻结意图(后续尽量不改;如果被迫要改,同步更新文档)。

**范围**:
- 审查 P1 公开 API:`Engine::Open / Put / Get / Delete / Close` 签名、`Options` / `Status` 类型
- `Options` 形态审查:当前 `DeviceConfig { path }` 是否够用、是否需加 reserved 字段
- `Status` 错误码空间评估:六段 × 1000 是否够后续阶段(WAL / io_uring / SPDK / 多 device)
- Engine 承诺语义审查:析构自动 Close、Put 部分写、Open 幂等
- 输出**公开 API 符号清单 + 冻结声明**文档
- **不实现并发安全**;P2–P6 单线程访问,多线程语义在 P7
- **不做 Forward-compat PoC**:reactor / 多 device / 零拷贝 / recovery 的验证推迟到各自功能实装后

**退出条件**:公开 API 符号清单审阅通过 + 冻结声明文档输出。冻结为设计意图声明(尽量保证),非绝对约束——全部完工发布后才是严格约束。

---

### P3 — IoBackend 与 MetaIndex 抽象层

**状态**：✅ 已实施（P3M4 收敛通过；详见 [doc/P3/P3M4_convergence_design.md](doc/P3/P3M4_convergence_design.md)）

**目标**:把"I/O 路径"和"索引数据结构"两个可替换组件同时抽象化,为后续多后端 / 多索引实现做准备。

**范围**:
- **`IoBackend` C++20 concept**:
  - 绑定到一个 device
  - 同步方法:`int32_t Write(block_idx, buf)` / `int32_t Read(block_idx, buf)`——无异步 / 无 poll 模型
  - `SyncIoBackend` 完整实现(O_DIRECT + pread / pwrite,包装 P1 已有的 WriteBlock / ReadBlock)
  - P4 io_uring / P10 SPDK 各自实装时内部异步、对 Engine 表现为同步(D17)
- **`MetaIndex` C++20 concept**:
  - 方法:`Insert` / `Lookup` / `Delete` / `Size` / `Contains` / `ForEach` / `WriteSnapshot` / `LoadSnapshot`
  - `HashMetaIndex` 实现(包装 `unordered_map`;ForEach / WriteSnapshot / LoadSnapshot 为空壳,P5 实装)
  - 契约测试套件(任何实现都要通过)
- CMake:`CABE_IO_BACKEND` 与 `CABE_META_INDEX` 变量编译期分派生效
- Engine 切换到两层抽象,功能等价于 P2
- **不做**:BufferHandle(P8) / 伪 SPDK(P10) / 异步接口(P4)

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

### P4.5 — FreeList 改造

**目标**:朴素 FreeList → 三容器轮换 + shard 内升序分配 + 异步 sort + TRIM。

**范围**:
- 三容器(active / sorting / recycled)数据结构,per-device
- `pop_back` 严格升序分配(shard 内)
- 后台 sort worker
- `Release` 路径同步 TRIM(`BLKDISCARD`)
- `RebuildFromActive(span<BlockId>)` 接口,为 P5 recovery 服务
- Stats 观察接口

---

### P5 — WAL + 崩溃恢复 + Snapshot/Truncate + 最小 Metrics

**目标**:持久化与 recovery 上线;WAL 不再无限增长。

**范围**:
- `WalWriter` per-device 实现
- WAL 帧格式:
  - 帧头 8 字节(D13)
  - 双 CRC:`header_crc` + `payload_crc`(CRC32C,D14)
  - 4 KiB 对齐
  - `entry_type`:`PutCommit` / `Delete` 占两值,其余预留
- WAL entry payload:
  - PutCommit: `key_len + key + BlockId + crc + timestamp`
  - Delete: `key_len + key + timestamp`
- commit 顺序:Data(FUA) → WAL(fsync) → Index(D11)
- Recovery per-device 并行:扫 WAL → 重建 MetaIndex → `FreeList::RebuildFromActive`
- **Snapshot + WAL truncate**(D15):
  - `MetaIndex` 接口包含 `WriteSnapshot` / `LoadSnapshot`
  - 触发策略:WAL 大小阈值 / 周期性(可调)
  - snapshot 文件头含 `version + entry_count + footer_crc`
  - 切换 snapshot 后旧 WAL 区间可截断
  - Recovery = LoadSnapshot + replay WAL since snapshot 位置
- 外置 WAL 设备(`DeviceContext.wal_device_path`)
- Crash injection 测试矩阵
- **最小 Metrics 接口**:counter + histogram,per-device label,P12 才导出

---

### P6 — Group Commit

**目标**:WAL fsync 合并,提升并发写吞吐。

**范围**:
- WAL writer 改造为"积攒帧 + 共享 flush"
- leader / follower 协议或 batching window(设计稿二选一,推荐 leader/follower)
- in-flight 帧的并发协调(lock-free MPSC queue)
- tail latency 控制(batch size + 最长等待时间上限)
- 失败语义:fsync 失败整 batch 同步返回失败

**性能回归红线**:p99 单 Put 延迟相对 P5 劣化 ≤ 20%。

---

### P7 — Reactor 并发模型 + 无锁多线程 + 多 device 端到端

**目标**:per-reactor 状态分区 + 消息传递,**无任何 mutex**;公开 API 自此冻结;多 device 端到端跑通。

**范围**:
- Reactor 抽象与线程模型:每 device R 个 reactor(初期 R = 1)
- **两级 hash 路由**:`device = hash(key) % N`,`reactor_within = (hash(key) / N) % R`
- 每 reactor 独占:`(IoBackend, FreeList partition, MetaIndex partition, WAL queue endpoint)`
- Reactor 间通信走 lock-free MPSC queue
- 公开 sync API → 内部异步:调用线程投递 op,挂起在 eventfd / futex
- io_uring SQPOLL / DEFER_TASKRUN 评估

**Milestone 拆分**:
- **M1**:单 device 多线程跑通(lock-free 路径)
- **M2**:2 device 端到端 Put / Get / Delete / Recovery 跑通
- **M3**:单 device 故障隔离行为(模拟一个 device 写失败,其他不受影响)

**性能回归红线**:
- 单线程 p50 延迟相对 P6 劣化 ≤ 10%
- 多线程峰值 QPS 相对 P6 单线程 ≥ 70% × N(70% 线性扩展)

---

### P8 — 零拷贝写入路径(主路径化)

**目标**:零拷贝成为默认 Put 路径;不对齐 buffer 隐性 fallback。

**范围**:
- 用户 buffer 通过 cabe 提供的 allocator 分配(从 per-device registered buffer pool 取)
- 对齐要求:1 MiB strict
- 不满足对齐自动走 copy 路径,API 不分裂、不报错
- io_uring registered buffer 协议升级
- 性能对比 bench:对齐 vs 非对齐 buffer
- 文档:使用约束 + cabe allocator 用法

---

### P9 — B+ 树索引学习路径

**目标**:在 `MetaIndex` 抽象层下实现 `BPlusTreeMetaIndex`,作为学习驱动的可选实现。

**学习驱动**:此阶段的主要目的是**掌握 B+ 树这一基础数据结构**,而非工程必要性。生产路径仍可保留 `HashMetaIndex`。

**范围**:4 个 milestone:

- **M1:基础结构**(4–6 周)
  - `IndexNode`(4 KiB 页,binary search,entry 数组)
  - `BPlusTree<Key, Value>` 模板
  - Insert / Lookup / Iterate
  - 不做 Delete、合并、并发、持久化
  - 测试:10⁷ entry 插入 + 全量查找,与 `std::map` 对照功能等价

- **M2:Delete + 合并 / 借用**(2–3 周)
  - Delete 操作 + 兄弟节点借用 / 合并
  - 测试:10⁸ 次随机 Insert/Delete,树高保持 O(log n)

- **M3:Snapshot / Restore**(2–3 周)
  - `WriteSnapshot(file)` / `LoadSnapshot(file)`
  - **Snapshot 格式 v2**(D15 的 version 字段自动选择)
  - 测试:crash injection during snapshot

- **M4:接入 Engine + 对照 bench**(2 周)
  - CMake `CABE_META_INDEX=bplustree` 切换
  - 对照 bench:Put / Get / Delete / Recovery
  - `doc/p9_index_comparison.md`:实测数据 + 结论
  - **决策**:基于 bench 决定最终默认实现

**性能可接受范围**(不影响默认决策的前提):
- 点查询 p50 相对 hashmap 劣化 ≤ 3 倍
- 内存占用相对 hashmap 不劣化(预期更优)

---

### P10 — SPDK 后端

**目标**:加 `SpdkIoBackend`,绕过内核态。

**范围**:
- 每 `(device, reactor)` 一个 SPDK 后端实例
- `BufferHandle` 内存来源切换到 SPDK pool(hugepage)
- SPDK reactor 与 cabe reactor 整合(同线程上 poller + 业务)
- 容器化 / hugepage 部署文档(N × R × pool_size 计算公式)
- 与 io_uring 后端 bench 对照

---

### P11 — 多 NVMe 规模化与隔离验证

**目标**:验证多 device 大规模部署可用(N ≥ 8);架构改造在 P7 已完成,本阶段只做规模化与运维验证。

**范围**:
1. `N ∈ {2, 4, 8}` 配置矩阵端到端测试
2. key 分布均衡性测量(xxh3 在真实 key 分布下的负载偏斜)
3. 多 device 聚合带宽 bench(单盘极限 → N 盘聚合的线性度)
4. 单 device 故障隔离深度测试(写失败、读失败、整盘掉线)
5. 运维文档:设备列表配置、故障处置、N 不可变的明确告知

**显式不做**:
- 跨 device 事务 / 原子 Put
- 热添加 / 热移除 device
- key 在 device 间的迁移 / rebalance

---

### P12 — 可观测性导出与运维工具

**目标**:把 P5 已经接入的 Metrics 接口导出为生产格式;运维工具链补齐。

**范围**:
- Metrics 导出(Prometheus / OpenMetrics),per-device label + 全局聚合
- 慢操作日志(可配置阈值)
- 健康检查 API(`Engine::HealthCheck()`,per-device 状态)
- 命令行工具:
  - `cabe-info`:查看 device 配置、容量使用、reactor 数量
  - `cabe-fsck`:离线一致性检查(WAL ↔ MetaIndex ↔ FreeList)
  - `cabe-dump`:dump key 列表或某 key 的 `ValueMeta`
- 运维手册 + 压测方法论文档

---

## 六、阶段间衔接约定

每阶段完成需提交:

1. 代码合入主分支,**四档 sanitizer CI 全绿**(P4+ TSAN 与 io_uring 组合除外)
2. 阶段设计稿 `doc/pN_xxx_design.md` 更新为"已实施",含取舍记录
3. bench 归档到 `bench/baselines/pN_xxx.json`
4. **性能回归红线检查**(阶段内明文列出)
5. README Roadmap 表对应阶段标记完成
6. 下一阶段设计稿启动(空文件 + 范围草稿)

---

## 七、关键约束(贯穿全期)

- Linux only,Fedora 43+ / 内核 6.16+
- C++20,GCC 15+ 或 Clang 20+
- TSAN 支持(P4+ io_uring 组合除外)
- 裸设备语义,不创建 / 不 truncate / 不 unlink 设备节点
- 健壮运行环境假设,不为内核 bug / 设备掉线做应用层兜底
- **任何阶段不得触发公开 API 破坏**;评估为必须则升级为 v2.0 候选,独立立项
- **N(设备数)在 Open 时固定**,运行期不可变;变更等同 v2.0

---

## 八、术语表

| 术语 | 层 | 定义 |
|---|---|---|
| **value** | 数据层 | 用户传入 / 取出的字节负载,大小恒为 `kValueSize`(1 MiB) |
| **`kValueSize`** | 常量 | 1 MiB = 1048576 字节 |
| **`BlockId`** | 设备层 | 物理寻址:`device_id:8 \| block_idx:56`,字节偏移 = `block_idx << 20` |
| **device** | 设备层 | 一个 NVMe 块设备(`/dev/nvmeXnY`),Engine 内 `vector<DeviceContext>` 一员 |
| **block** | 设备层 | device 上一个 1 MiB 物理区域;**1 block 存 1 value** |
| **`ValueMeta`** | 数据层 | 内存索引中关于一个已存 value 的元数据 `{BlockId, crc, timestamp, state}` |
| **`MetaIndex`** | 数据层 | `key → ValueMeta` 的索引,abstraction(D21) |
| **WAL** | 持久化层 | 写前日志,per-device,所有元数据变更的真相源 |
| **snapshot** | 持久化层 | MetaIndex 的周期性磁盘镜像,用于 WAL truncate 与加速 recovery |
| **N** | 配置 | 设备数,Open 时固定 |
| **R** | 配置 | 每 device 的 reactor 数,Open 时固定,初期 = 1 |
| **reactor** | 并发层 | 独占 `(IoBackend, FreeList partition, MetaIndex partition)` 的执行单元 |

---

## 九、决策汇总表(附录)

完整内容见第三节(D1–D26)。此处列出每个决策的阶段绑定:

| 决策 | 简述 | 锁定阶段 |
|---|---|---|
| D1 | value 严格 1 MiB | P0 |
| D2 | 数据设备无 header | P0 |
| D3 | 元数据仅 RAM + WAL | P0 |
| D4 | 命名分层(BlockId / ValueMeta) | P0 |
| D5 | BlockId 8/56 编码 | P0 |
| D6 | xxh3 路由 | P0 |
| D7 | hash(key) % N 路由 | P2 |
| D8 | N 不可变 | P2 |
| D9 | R 不可变,初期 R=1 | P2 |
| D10 | FUA value durability | P5 |
| D11 | commit 顺序 Data→WAL→Index | P5 |
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
| D22 | 默认实现 hashmap | P3 |
| D23 | 编译期切换 | P3 |
| D24 | 零拷贝主路径 | P8 |
| D25 | API 冻结于 P2 | P2 |
| D26 | 性能回归红线 | 各阶段 |
