# Cabe P0-M7 设计：阶段收敛与状态固化

> 本里程碑作为 P0 阶段收敛节点：撰写阶段收敛稿（`P0M7_convergence_design.md`），实装微基准
> 脚本（`scripts/run-bench.sh`）并归档基线数值（`bench/baselines/p0_utilities.json`），清理
> 评审残留 7 项 LOW 防御性问题，落下 P1 / P2 阶段占位索引，并把 `ROADMAP.md`、根 `README.md`、
> `doc/P0/README.md` 与各 P0M1–M6 设计稿的状态字串一并推到锁定态。完成后 P0 整体出口。
>
> **本文为详细设计**。技术细节采用薄索引形态：每章只列锁定结论 + 链回 P0M1–M6 对应章节，
> 不复制 schema、错误码、CMake 选项等具体定义（详见各上游设计稿）。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P0 / M7 |
| 状态 | **完成稿（待 owner 终审）** —— 七项决策经拷问式追问拍板（见 §3） |
| 上游依赖 | M1–M6 全部完成（骨架 / schema / 错误码 / hash / 测试与微基准框架 / 本地组合矩阵脚本） |
| 下游依赖本里程碑 | P0 阶段出口；P1 / P2 启动闸门 |
| 关联约束 | ROADMAP M7 字面范围；M1–M6 各稿留给 M7 的同步钩子；M6-D1（持续集成推迟）已锁定 |
| 退出判定 | 见 §10（六条） |

---

## 1. 目标与范围

### 1.1 目标

1. 把 P0 阶段全部技术结论凝练为单一收敛稿入口（`P0M7_convergence_design.md`），便于 P1+
   读者从这里跳到各上游详细稿。
2. 实装 `scripts/run-bench.sh`，跑出 `crc32` / `hash` 微基准基线并归档到
   `bench/baselines/p0_utilities.json`，使后续阶段可一键复现基线、做回归对照。
3. 清理 P0M6 评审遗留的 7 项 LOW 防御性问题（编号 #9–#15），让 P0 出口零已知债务进入 P1。
4. 落下 P1 / P2 阶段占位索引（`doc/P1/README.md`、`doc/P2/README.md`），明确启动条件。
5. 把 `ROADMAP.md`、根 `README.md`、`doc/P0/README.md`、各 P0M1–M6 设计稿的状态字串与
   旧版 CI 相关字面一并推到 P0 锁定态，做到全仓状态一致。

### 1.2 交付范围（本里程碑产出）

1. **`doc/P0/P0M7_convergence_design.md`**：本设计稿（薄索引形态）。
2. **`doc/P1/README.md` + `doc/P2/README.md`**：阶段占位索引（中等深度——含 ROADMAP 范围
   摘要、已知决策点候选、启动条件）。
3. **`scripts/run-bench.sh`**：双工具链 Release 档微基准运行脚本（详见 §5.1）。
4. **`bench/baselines/p0_utilities.json`**：`crc32` × 3 长度 + `hash` × 3 长度 × 双工具链
   × 5 次重复取中位数的基线归档（详见 §5.2）。
5. **评审残留修复**（详见 §6）：
   - #9 `util/crc32.cpp` `detail::HardwareCRC32C_x86` 加 `[[gnu::target("sse4.2")]]`
   - #10 `scripts/run-coverage.sh` 把 `llvm-cov export` JSON 解析改用 `jq`
   - #11 `scripts/run-coverage.sh` 三元写法改 if-then-else
   - #12 `bench/CMakeLists.txt` 显式链接 `cabe::common`
   - #13 `test/CMakeLists.txt` + `bench/CMakeLists.txt` 给 `find_package` 加版本下限
   - #14 `scripts/setup-dev.sh` `VERSION_ID` 算术展开前剥小数点
   - #15 `scripts/setup-dev.sh` `--ci` 模式跳过 `liburing` / `io_uring` 检查
6. **状态同步**（详见 §8）：各 P0M1–M6 稿头部状态字串 → "✅ 已锁定（P0M7 收敛）"；
   `doc/P0/README.md` 退出条件 6 条 + M7 条目更新；根 `README.md` P0 行 → "✅ 完成"；
   `ROADMAP.md` M7 段重写 + P0 状态字串 → "已实施" + 字面 `doc/p1_core_design.md` /
   `doc/p2_api_freeze.md` 改为 `doc/P1/README.md` / `doc/P2/README.md`。

### 1.3 推迟范围（明确推迟，标注落点）

| 推迟项 | 落点 | 原因 |
|---|---|---|
| **P0 阶段厚整合稿（重写形态）** | **未来全部完工后** 单独立项 | M7-D2 锁定：薄索引形态足够 P0 出口；厚整合稿在全部完工后作为对外宣讲材料更合适 |
| P1 / P2 各自里程碑划分与决策梳理 | **各阶段启动时** | M7 只放占位索引，不越界进 P1/P2 设计活；待 owner 触发 `/grill-with-docs P1` 时正式启动 |
| 持续集成（CI）工作流 | **P0 之外单独立项**，待仓库托管确定 | M6-D1 已锁定，M7 仅做 ROADMAP / README 字面同步 |
| `.gitignore` 维护（含 `/build<后缀>/` / `/build-bench/` 落入约定） | **owner 自管** | 临时区现行 `.gitignore` 的 `/build*/` 通配已覆盖；M7 不擅改 |

---

## 2. 现状盘点（读码 + 读文档结论）

- **M1–M6 全部完成并已工作区落盘**：骨架 / schema / 错误码 + 日志 / 路由 hash /
  测试·微基准框架 / 本地组合矩阵与覆盖率脚本均已实装；最近一轮跑分 `run-tests.sh --asan && run-tests.sh --tsan && run-tests.sh --ubsan && run-tests.sh --release`
  四档全过，`run-coverage.sh --strict` 行覆盖率 98.4%（21 个测试用例，含评审修复后新增的
  `ValueMeta.MemcpyRoundTrip` 与 `Hash.RouteInRange` 公式回归断言）。
- **P0M6 评审残留 7 项 LOW**（#9–#15）：评审已核实，未在 M6 范围内修复，由本里程碑收口
  （M7-D4 已锁定）。
- **当前与 ROADMAP / 子稿字面有出入**：`doc/P0/README.md` 退出条件第 2 / 4 条仍是旧版
  CI 字面（"四档 CI 全绿" / `P0_infra_design.md`）—— 与 M6-D1 已改的 `ROADMAP.md` /
  P0M6 设计稿不同步，本里程碑收口。
- **bench 框架已就位但基线未归档**：`bench_crc32` / `bench_hash` 可执行可构建运行
  （M5 完成）；`bench/baselines/` 空目录在 M5 设计稿里曾被列为"待建"，**当前缺失**
  （评审已识别）；本里程碑首次建立目录并归档基线 JSON。
- **`run-bench.sh` 不存在**：M6-D5 已锁定"bench 独立脚本"，但 M6 范围内未写；本里程碑
  顺手交付（M7-D3 决策）。
- **P1 / P2 阶段目录不存在**：根 `doc/` 下当前只有 `doc/P0/`；本里程碑新建 `doc/P1/` 与
  `doc/P2/` 并放入占位索引。

---

## 3. 关键决策（owner 已拍板）

下表汇总拷问式追问得出的七项决策。详细论证见后续各节。

| 编号 | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| **M7-D1** | 阶段收敛稿命名 `doc/P0/P0M7_convergence_design.md`（单稿、与 M1–M6 命名风格一致），不改 M1–M6 文件名；ROADMAP 原字面 `doc/p0_infra_design.md` 反向改 | 单稿 `P0_infra_design.md`（破坏当前风格） / 改 M1–M6 全部用 `infra` 主题词（主题词失去区分意义） | 当前风格 `P<阶段>M<里程碑>_<主题>_design.md` 已固化；改 M1–M6 命名收益为零、成本极高 | 锁定 |
| **M7-D2** | 收敛稿覆盖深度：**薄索引**形态（每章摘要 + 链回 P0M1–M6 对应章节）；未来全部完工后再做一次厚整合稿 | 厚收敛稿（重写 schema/错误码/术语全文，双重维护） / 纯状态同步稿（少阶段全景入口） | 与 cabe "每稿独立单点权威" 精神一致；零重复维护；P1+ 读者从 §4 入口跳到细节 | 锁定 |
| **M7-D3** | bench 基线归档全方案：实装 `scripts/run-bench.sh`（与 `run-tests.sh` 接口同风格）；`build-bench/<工具链>-release/` 目录；双工具链 × Release × 5 次重复 × 取中位数；JSON schema 含环境/构建/方法/结果四块 | M7 不写脚本只手动一次跑（基线失去可复现性） / 基线推到 P1+ 单独立项（M7 出口残缺） | baseline 必须可一键复现；脚本量级与 `run-tests.sh` 相当 | 锁定 |
| **M7-D4** | 评审残留 #9–#15 共 7 项 LOW 全部在 M7 收口；进 P1 零债务 | 仅修无脑 1 行级（留 3 项尾巴） / 全部推迟到 P1+ | 总成本 ≈ 1 小时；M7 是收敛节点，固化前清场最合适；与 cabe 工作流"严谨"风格一致 | 锁定 |
| **M7-D5** | P1 / P2 占位：`doc/P1/README.md` + `doc/P2/README.md`（索引型，中等深度——含 ROADMAP 范围摘要、已知决策点候选、启动条件、里程碑划分预案）；ROADMAP 字面 `doc/p1_core_design.md` / `doc/p2_api_freeze.md` 反向改 | 阶段级单稿 `P1_core_design.md`（cabe 无此风格先例） / 厚预梳理稿（违反"先决策梳理后写"原则） / 完全不放 | 与 `doc/P0/README.md` 对称；既给 P1 启动加速指引，又不越界进 P1 设计活 | 锁定 |
| **M7-D6** | 状态字串收敛：各 P0M1–M6 稿头部 → "✅ 已锁定（P0M7 收敛）"；`doc/P0/README.md` 退出条件改写为 6 条（含基线归档新增条）+ M7 条目更新；根 `README.md` 表格 P0 → "✅ 完成"；`ROADMAP.md` M7 段按本设计稿重写 + P0 状态字串 → "已实施" | 状态字串用 "✅ 已锁定"（失溯源） / "✅ 完成"（≠锁定） / "已固化"（术语陌生） | "已锁定（P0M7 收敛）" 明确收敛来源里程碑，便于未来追溯；与各稿头部"待 owner 终审"的语义切换清晰 | 锁定 |
| **M7-D7** | M7 退出条件：六条（文档撰写 / 微基准基础设施与基线 / 评审残留全清 / 回归实证 / 状态同步 / owner 终审）—— 见 §10 | 简化为 4 条（合并后追溯性下降） / 不设独立退出条件（M7 自身节奏丢失） | 一一对应 D1–D6；可逐项验证；与 M1–M6 各稿的退出条件章节风格一致 | 锁定 |

---

## 4. 收敛技术索引（薄索引形态）

按 ROADMAP M7 字面所列范围分章。每章只列结论 + 跳转入口，**不复制定义**。

### 4.1 数据 Schema（详 [P0M2_schema_design.md](P0M2_schema_design.md)）

- **`kValueSize`**：1 MiB 定长 value（D1 锁定，跨阶段不变）。
- **`BlockId`**：`uint64_t`，高 8 位 `device_id`，低 56 位 `block_idx`；`logical_byte_offset() = block_idx * kValueSize`（D5，逻辑偏移；P5 起物理偏移由 IoBackend 加 kDataRegionOffset）。
- **`DeviceId`**：`uint8_t`，取值 [0, 256)。
- **`DataView` / `DataBuffer`**：`std::span<const std::byte>` / `std::span<std::byte>`，设备上裸字节
  视图（D2 / D4）。
- **`ValueMeta`**：`sizeof == 24`，`alignof == 8`，字段顺序 `block(8) / timestamp(8) / crc(4) / state(1) / reserved[3]`
  ——为达成 24 字节布局并保证 memcpy 序列化的确定性而非 ROADMAP 字面顺序；论证详见
  [P0M2 §3 / §6](P0M2_schema_design.md)。
- **`ValueState`**：`uint8_t` 枚举，`Active = 0` / `Deleted = 1`。
- **WAL 帧头占位**：8 字节布局常量（`kWalFrameHeaderSize` 等），P5 真实编解码时移入 `wal` 模块。
- **完整 `static_assert` 链**：`BlockId` 8 字节、`ValueMeta` 24 字节 / 8 字节对齐 / trivially-copyable
  / standard-layout —— 编译期不可绕过。

### 4.2 错误码段位（详 [P0M3_errorcode_logger_design.md](P0M3_errorcode_logger_design.md)）

- **段位规划**：六段，每段容量 `kSegmentSize = 1000`，向更负方向编号；段间不重叠（`static_assert` 守护）。

  | 段 | 基址 | 范围 | 含义 |
  |---|---|---|---|
  | memory | `-100000` | [-100999, -100000] | 内存 / 调用错（已有四个码） |
  | io | `-101000` | [-101999, -101000] | I/O 错（P4+ 填） |
  | index | `-102000` | [-102999, -102000] | 索引错（P3+ 填） |
  | wal | `-103000` | [-103999, -103000] | WAL 写错（P5+ 填） |
  | engine | `-104000` | [-104999, -104000] | 引擎装配错（P1+ 填） |
  | wal_recovery | `-105000` | [-105999, -105000] | WAL 恢复错（P5+ 填） |

- **段内编号助手**：`InSeg(base, n)` constexpr，编译期可校验越段。
- **已分配的 memory 段码**：`kMemNullPointer / kMemEmptyKey / kMemEmptyValue / kMemInsertFail`。
- **`kSuccess = 0`**：唯一表示成功的常量。

### 4.3 日志接口（详 [P0M3 §7](P0M3_errorcode_logger_design.md)）

- **形态**：纯头宏 + stderr，`common/logger.h`，无 `.cpp`。
- **级别**：`DEBUG / INFO / WARN / ERROR / FATAL`（默认 `WARN`，进程启动读环境变量
  `CABE_LOG_LEVEL` 一次即缓存）。
- **宏接口**：`CABE_LOG_DEBUG / INFO / WARN / ERROR / FATAL`，printf 风格；fmt 必须是字符串字面量。
- **格式串硬约束**（评审 #2 在 M6 已修复 + M7 维持）：根 `CMakeLists.txt` 在 `cabe_flags` 加
  `-Werror=format -Werror=format-security`；fmt 含裸 `%` 不传参在编译期阻断。
- **输出格式**：`[LEVEL][tid][file:line] message`，单次 `fprintf`，依赖 stdio 的 FILE 内部锁保证整行原子。

### 4.4 路由 hash（详 [P0M4_hash_design.md](P0M4_hash_design.md)）

- **算法**：XXH3 64-bit，固定 seed 0（D6 冻结——更改算法或 seed 等同 v2.0 破坏）。
- **入口**：`cabe::util::Hash(DataView)` / `cabe::util::Hash(std::string_view)`，两者等价（字面 byte 一致）。
- **路由函数**：`RouteToDevice(key, n_devices) -> DeviceId`；公式 `Hash(key) % n_devices`（D7）。
- **接收范围**：`n_devices ∈ [1, 256]`；Debug 由 `assert` 拦截，Release 由 `if (n_devices == 0) std::abort();`
  兜底（评审 #1 在 M6 已修复）。
- **库形态**：单文件 vendored `third_party/xxhash/xxhash.h`（v0.8.2），`XXH_INLINE_ALL` 仅在
  `util/hash.cpp` 这一个 TU 内展开。

### 4.5 工程骨架与 CMake 选项（详 [P0M1_skeleton_design.md](P0M1_skeleton_design.md)）

- **平台**：仅 Linux（Fedora 43+），`CMakeLists.txt` 与 `common/structs.h` 双重护栏。
- **工具链**：GCC 15+ / Clang 20+，C++20，`CMAKE_CXX_EXTENSIONS OFF`（使用 `-std=c++20` 非 `gnu++20`）。
- **CMake 版本**：3.30+；生成器默认 Ninja。
- **默认构建类型**：单配置生成器下未指定时落 `Release`。
- **构建选项**（汇总）：

  | 选项 | 默认 | 取值 | 引入里程碑 |
  |---|---|---|---|
  | `CABE_SANITIZER` | `none` | `none / address / thread / undefined` | M1 |
  | `CABE_IO_BACKEND` | `sync` | `sync / io_uring / spdk`（M1 仅声明，分派在 P3 / P4 / P10） | M1 |
  | `CABE_META_INDEX` | `hashmap` | `hashmap / bplustree`（M1 仅声明，分派在 P3 / P9） | M1 |
  | `CABE_BUILD_TESTS` | `OFF` | `ON / OFF` | M1 声明 / M5 启用 |
  | `CABE_BUILD_BENCH` | `OFF` | `ON / OFF` | M1 声明 / M5 启用 |
  | `CABE_WERROR` | `OFF` | `ON / OFF`（全量 `-Werror`） | M1 |
  | `CABE_COVERAGE` | `OFF` | `ON / OFF`（gcov / llvm-cov 插桩） | M5 |

- **默认硬错告警**（`cabe_flags` 不可绕过）：`return-type / uninitialized / implicit-fallthrough /
  format / format-security`（后两条评审 #2 在 M6 顺路升级）。
- **目标拓扑**：`cabe_flags`（INTERFACE）→ `cabe_common`（INTERFACE）→ `cabe_util`（STATIC）→
  测试 / 微基准目标。

### 4.6 测试与微基准约定（详 [P0M5_test_bench_design.md](P0M5_test_bench_design.md)）

- **单元测试**：GTest（系统库优先，未命中走 `FetchContent` v1.15.2 兜底）；每个被测模块一个测试
  可执行；`gtest_discover_tests(... DISCOVERY_TIMEOUT 60)`（评审 #6 在 M6 已加 timeout）。
- **测试用例数**（当前）：21 个，覆盖 `crc32 / hash / util / cpu_features / structs / error_code`。
- **微基准**：google-benchmark（系统库优先，`FetchContent` v1.9.1 兜底）；hot loop 含
  `benchmark::DoNotOptimize` + `benchmark::ClobberMemory()` 防 hoist（评审 #7 在 M6 已加 barrier）。
- **覆盖率门槛**：`util/` + `common/` 行覆盖率 ≥ 80%；最近一次实测 98.4%（60 / 61 行）。
- **覆盖率插桩**：`CABE_COVERAGE=ON` —— GCC 走 `--coverage`，Clang 走
  `-fprofile-instr-generate -fcoverage-mapping`。

### 4.7 本地组合矩阵脚本（详 [P0M6_test_scripts_design.md](P0M6_test_scripts_design.md)）

- **`scripts/run-tests.sh`**：单次调用跑一个配置（`--asan` / `--tsan` / `--ubsan` / `--release` / `--debug`）；
  支持 `--filter` / `--clean` / `--jobs` / `--backend=`；失败时 dump 该档 log；构建目录 `build<后缀>/`
  （如 `build-asan/`）。**四档验证**：`run-tests.sh --asan && run-tests.sh --tsan && run-tests.sh --ubsan && run-tests.sh --release`。
- **`scripts/run-coverage.sh`**：默认 GCC + gcovr 路径；`--compiler=clang++` 走 llvm-cov 路径
  （`llvm` 未列入 setup 必装清单，按需自装）；`--strict` 行覆盖率 <80% 时退出码 1。
- **`scripts/setup-dev.sh`**：Fedora 43 包管理，REQUIRED_PKGS 含 `gcovr`（M6-D8）。
- **持续集成**：M6-D1 锁定推迟，待仓库托管确定后单独立项；本里程碑仅做字面同步。

### 4.8 术语表（单点权威）

| 术语 | 释义 |
|---|---|
| **Cabe** | 项目代号 / 命名空间 `cabe`，固定 value 长度的键值存储引擎，直接操作 NVMe 裸块设备 |
| **kValueSize** | 单 value 固定大小 = 1 MiB，全局编译期常量，跨阶段不可改 |
| **BlockId** | 物理块地址，`uint64_t` 高 8 位 `device_id` + 低 56 位 `block_idx` |
| **DeviceId** | 设备编号，`uint8_t`，取值 [0, 256) |
| **DataView / DataBuffer** | 只读 / 可写裸字节视图，`std::span<const std::byte>` / `std::span<std::byte>` |
| **ValueMeta** | value 元数据（block / timestamp / crc / state / reserved），24 字节，可 memcpy 序列化 |
| **ValueState** | value 状态枚举（Active / Deleted），`uint8_t` |
| **CRC32C** | Castagnoli 多项式 CRC32，硬件路径 SSE4.2，软件 fallback 256 表 |
| **XXH3** | 路由 hash 算法（64-bit），固定 seed 0 |
| **路由 hash / 路由函数** | 按 key 选 device 的函数 `RouteToDevice` |
| **本地组合矩阵 / 四档** | 单次调用跑一个配置（asan / tsan / ubsan / release），四档逐一验证 |
| **检测器** | sanitizer 中译；具体三种以缩写形式出现（ASAN / TSAN / UBSAN）|
| **微基准** | google-benchmark 框架跑出的单函数性能数 |
| **行覆盖率** | gcov / llvm-cov 算出的"被执行行数 / 总可执行行数" |
| **覆盖率门槛** | 80%，cabe P0 退出条件第 3 条 |
| **持续集成（CI）** | M6-D1 决策推迟，不在 P0 范围 |
| **IoBackend** | I/O 抽象层（P3+ 引入），同步 / `io_uring` / SPDK |
| **MetaIndex** | 索引抽象层（P3+ 引入），哈希 / B+ 树 |
| **WAL** | 写前日志，P5+ 引入 |
| **Snapshot** | 快照与 WAL 截断，P5+ 引入 |
| **Reactor** | 并发模型（P7+ 引入），无锁多线程 |
| **TU**（Translation Unit）| C++ 编译单元，一个 `.cpp` 文件加全部 `#include` 后的结果 |
| **SIOF**（Static Init Order Fiasco）| 跨 TU 静态对象初始化顺序未定义问题；cabe 用函数内 static 规避 |
| **vendored** | 第三方库以源码内嵌方式纳入（如 `third_party/xxhash/xxhash.h`），不走系统包管理 |

---

## 5. 微基准基线归档方案（M7-D3 详细）

### 5.1 `scripts/run-bench.sh` 设计

#### 5.1.1 命令行接口

```
用法: scripts/run-bench.sh [选项]

工具链选项:
  --compiler=NAME   只在指定工具链上跑（g++ / clang++ / all，默认 all）

归档选项:
  --baseline=PATH   把跑出的中位数结果写到 JSON 文件（按 §5.2 schema）；
                    不传则仅 stdout 打印 google-benchmark 原始报告

冗余度:
  -v, --verbose     输出每格完整 cmake / build / 微基准日志
                    默认精简：每格一行 OK / FAIL，失败时 dump 该格 log

其他:
  -h, --help        输出本用法

退出码:
  0  全部 OK；若传 --baseline 则 JSON 写入成功
  1  任一格 FAIL；或 --baseline 写入失败
  2  参数错误
```

#### 5.1.2 行为细节

**每格构建命令**（伪代码）：
```bash
cmake -S "$ROOT" -B "build-bench/${compiler_short}-release" -G Ninja \
      -DCMAKE_CXX_COMPILER="$compiler" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCABE_BUILD_BENCH=ON
cmake --build "build-bench/${compiler_short}-release"
```

- `compiler_short`：与 `run-tests.sh` 一致，`g++ → gcc` / `clang++ → clang`
- 不开 `CABE_SANITIZER` / `CABE_COVERAGE`：检测器与覆盖率插桩会污染数值
- 不传 `CABE_BUILD_TESTS`：避免编译测试目标增加构建时间

**构建目录策略**：与 `run-tests.sh` 一致——`build-bench/<工具链>-release/` 源码树内，跑前清空、
跑完不清。`build-bench/` 目录名在 M6 §11 已预约。

**微基准运行**：
```bash
for bin in "$build_dir"/bench/bench_crc32 "$build_dir"/bench/bench_hash; do
    "$bin" --benchmark_repetitions=5 \
           --benchmark_report_aggregates_only=true \
           --benchmark_format=json > "$build_dir/$(basename $bin).json"
done
```

- `--benchmark_repetitions=5`：5 次重复，抑制单次波动
- `--benchmark_report_aggregates_only=true`：仅输出聚合（min / max / mean / median / stddev）
- `--benchmark_format=json`：原生 JSON 输出，便于后续解析归档

**失败语义**：与 `run-tests.sh` 一致——`set -uo pipefail` 不开 `-e`，逐段 `if !` 拦截，
单格失败记 `FAIL(stage)` 继续下一格。

#### 5.1.3 输出汇总

```
==== 汇总 ====
g++     RELEASE  : OK     (bench_crc32 + bench_hash, 5 reps each)
clang++ RELEASE  : OK
--------------
全部 2 格 OK
基线已写入: /home/cabe/bench/baselines/p0_utilities.json
```

### 5.2 `bench/baselines/p0_utilities.json` schema

```json
{
  "schema_version": "1.0",
  "milestone": "P0M7",
  "captured_at": "<YYYY-MM-DD，跑出当天>",
  "git_commit": "<git rev-parse --short HEAD>",
  "env": {
    "kernel": "<uname -r>",
    "cpu_model": "<from /proc/cpuinfo 第一行 model name>",
    "cpu_features_relevant": ["sse4.2"]
  },
  "build": {
    "type": "Release",
    "cmake_flags": ["-DCABE_BUILD_BENCH=ON"],
    "compiler_specific": {
      "g++":     "<g++ --version 第一行>",
      "clang++": "<clang++ --version 第一行>"
    }
  },
  "method": {
    "tool": "google-benchmark",
    "repetitions": 5,
    "aggregate": "median",
    "rationale": "5 次重复取中位数：抑制单次波动 + 比均值更稳健于离群"
  },
  "results": {
    "g++": {
      "bench_crc32/64":      { "items_per_second": <num>, "bytes_per_second": <num>, "cpu_time_ns": <num> },
      "bench_crc32/4096":    { "items_per_second": <num>, "bytes_per_second": <num>, "cpu_time_ns": <num> },
      "bench_crc32/1048576": { "items_per_second": <num>, "bytes_per_second": <num>, "cpu_time_ns": <num> },
      "bench_hash/16":       { "items_per_second": <num>, "bytes_per_second": <num>, "cpu_time_ns": <num> },
      "bench_hash/256":      { "items_per_second": <num>, "bytes_per_second": <num>, "cpu_time_ns": <num> },
      "bench_hash/1048576":  { "items_per_second": <num>, "bytes_per_second": <num>, "cpu_time_ns": <num> }
    },
    "clang++": { "<同上 6 项>": "..." }
  },
  "notes": "P0 收敛基线；P1+ 不强制不退步——仅作回归参考。完整方法见 P0M7_convergence_design.md §5。"
}
```

**字段约定**：
- `schema_version`：基线 schema 自身的版本号，未来若加 / 减字段递增。
- `milestone`：归档发生的里程碑标识。
- `captured_at`：归档当天 ISO 日期。
- `git_commit`：归档时仓库 HEAD 的 short SHA，便于回溯到当时代码状态。
- `env`：硬件 / 内核 / CPU 特性快照——cabe 的 CRC32C 走 SSE4.2 硬件路径，CPU 是否含 SSE4.2 直接
  决定基线数量级。
- `build`：构建配置 + 工具链版本；如果未来 GCC / Clang 升级使数值大变，可对比这一字段。
- `method`：跑法元数据——5 次中位数是 M7-D3 决策。
- `results`：分工具链分用例的核心数据；`items_per_second` / `bytes_per_second` / `cpu_time_ns`
  三个字段，覆盖吞吐与延迟两个维度。
- `notes`：归档语义提醒——cabe P0 阶段不强制后续阶段"不退步"，因为优化重心在后续阶段。

### 5.3 跑法与归档流程（M7 内一次性动作）

1. 工作区一次性跑 `./scripts/run-bench.sh --baseline=bench/baselines/p0_utilities.json`。
2. 脚本逐格清 `build-bench/<工具链>-release/`、cmake configure、cmake build、跑两个微基准
   可执行各 5 次取中位数、把结果合并到 JSON。
3. 跑完 owner 人工审阅 JSON：硬件 / 工具链版本是否合理、数量级是否预期内、是否需要再跑一次
   做交叉确认。
4. owner 接受后 commit。

---

## 6. 评审残留修复清单（M7-D4 详细）

7 项 LOW 防御性问题在本里程碑全部清场。每项给出具体修改位置 + 验证方式。

| 编号 | 文件 | 修改 | 验证 |
|---|---|---|---|
| **#9** | `util/crc32.cpp` line 105 | `detail::HardwareCRC32C_x86` 包装函数加 `[[gnu::target("sse4.2")]]` 属性，与内层一致 | `run-tests.sh --asan && ... --tsan && ... --ubsan && ... --release` 四档 PASS（不退步） |
| **#10** | `scripts/run-coverage.sh` line 163 | `llvm-cov export` 的 JSON 解析改用 `jq -r '.data[0].totals.lines.percent'`；`run-coverage.sh` 自检段加 `jq` 依赖（缺失则提示 `sudo dnf install jq`） | `--compiler=clang++ --strict` 实际跑通（前提是手装 `llvm` + `jq`） |
| **#11** | `scripts/run-coverage.sh` line 78 | `[[ ... ]] && ... \|\| ...` 三元改 if-then-else 显式分支 | `--compiler=g++ --strict` 仍 98.4% 通过；`bash -n` 通过 |
| **#12** | `bench/CMakeLists.txt` | `bench_crc32` / `bench_hash` 各 `target_link_libraries` 显式加 `cabe::common` | `cmake -B build-bench -DCABE_BUILD_BENCH=ON && cmake --build build-bench` 通过 |
| **#13** | `test/CMakeLists.txt` + `bench/CMakeLists.txt` | `find_package(GTest 1.14 REQUIRED CONFIG)` 与 `find_package(benchmark 1.8 REQUIRED CONFIG)` 加版本下限 | 配置阶段无 warning；老版本系统库会 fail-fast 触发 `FetchContent` 兜底 |
| **#14** | `scripts/setup-dev.sh` line 35 | `major="${VERSION_ID%%.*}"; (( ${major:-0} < 43 ))` —— 算术展开前剥小数点 | 手造 `VERSION_ID="43.1"` 跑，不报 syntax error |
| **#15** | `scripts/setup-dev.sh` line 89-126 | io_uring 段（pkg-config 版本检查 + sysctl 检查 + RLIMIT 检查）外层包 `if [[ "$CI_MODE" == "false" ]]`；`--ci` 模式跳过整段 | `bash -n` 通过；`--ci` 模式在无 `liburing-devel` 容器中不再 exit 1 |

**回归验证**：修复全部落入后再跑一次：
- `./scripts/run-tests.sh --asan && ./scripts/run-tests.sh --tsan && ./scripts/run-tests.sh --ubsan && ./scripts/run-tests.sh --release` — 期望 **四档 PASS**（21 个测试用例不退步）
- `./scripts/run-coverage.sh --strict` — 期望 **≥80%**（修复后用例数与覆盖率不变）
- `./scripts/run-bench.sh` — 期望 **2/2 OK** + baseline 写入

---

## 7. P1 / P2 占位稿规划（M7-D5 详细）

### 7.1 目录与命名

- 新建 `doc/P1/` 与 `doc/P2/` 阶段目录，各放一份 `README.md` 作为占位索引。
- 沿用 P0 目录约定：`P<阶段>M<里程碑>_<主题>_design.md`；里程碑划分在阶段启动时由决策梳理过程
  确定，当前 `README.md` 仅作索引 + 启动闸门 + 决策点候选清单。

### 7.2 `doc/P1/README.md` 内容大纲

1. **状态**：🚧 未启动（待 P0M7 收敛通过 + owner 确认启动）。
2. **阶段目标**（摘 ROADMAP §"P1 — 单线程版核心引擎"）：跑通完整 Put / Get / Delete 路径；
   单线程、无持久化、纯 RAM 索引。
3. **范围摘要**：5–8 行 bullet 抽 ROADMAP P1 段重点（`cabe::Options` / `cabe::Status` 公开类型骨架；
   `cabe::Engine` 五个公开方法；`DeviceContext` 内部结构；朴素 syscall + O_DIRECT；朴素 BufferPool）。
4. **里程碑文档清单**（占位）：表头同 P0，行内容写"待决策梳理划分 P1M1–Mn"。
5. **启动条件**：P0M7 收敛稿审阅通过 + owner 确认 + 用 `/grill-with-docs P1` 开第一个里程碑。
6. **已知决策点候选**（中等深度——为决策梳理过程提速）：
   - 公开 API 暴露面：仅 `Engine`，还是 `Engine` + `Options` + `Status` 一并？
   - `Engine` 是否支持移动 / 拷贝？
   - 错误传播：返回 `Status`、抛异常、还是两种风格混用？
   - `DeviceContext` 单例（per-Engine）还是多例（per-device）？
   - 朴素 BufferPool 的对齐策略：编译期常量 vs 运行时探测？
   - O_DIRECT 与 BufferPool 的协作：所有 IO 都过 BufferPool 还是部分直读？
7. **命名与目录约定**：链回 `doc/P0/README.md`。

### 7.3 `doc/P2/README.md` 内容大纲

1. **状态**：🚧 未启动（待 P1 完成）。
2. **阶段目标**（摘 ROADMAP §"P2 — 公开 API 契约冻结 + Forward-compat 论证"）：把 P1 的公开
   API 锁定 + 写 Forward-compat 论证 PoC。
3. **范围摘要**：5–8 行 bullet（API 冻结点、forward-compat 论证方法、API 版本号机制等）。
4. **里程碑文档清单**：占位"待决策梳理划分 P2M1–Mn"。
5. **启动条件**：P1 阶段所有里程碑完成 + owner 确认。
6. **已知决策点候选**：
   - API 版本号方案：语义化（major.minor.patch）还是单调整数？
   - Forward-compat 论证 PoC 形态：写一个未来场景的"假设 client"代码？
   - 错误码 ABI 是否一并冻结？
   - 公开类型的 ABI 兼容承诺范围（结构体布局 / 枚举值 / 函数签名）？
7. **命名与目录约定**：链回 `doc/P0/README.md`。

---

## 8. 状态同步动作清单（M7-D6 详细）

按文件分组列出全部要改的位置。

### 8.1 各 P0M1–M6 设计稿头部状态字串

**`doc/P0/README.md` 表格里 M1–M6 行**：状态列从 `完成稿，待 owner 终审` 改为 `✅ 已锁定（P0M7 收敛）`。

各 P0Mn 设计稿头部"§0 元信息"表格"状态"行（如有）：同步改为 `✅ 已锁定（P0M7 收敛）`。

### 8.2 `doc/P0/README.md` 退出条件 6 条（旧 5 条 + 新增 1 条）

原文：
```
1. GCC 15+ 与 Clang 20+ 双工具链 build 通过
2. ASAN / TSAN / UBSAN / Release 四档 CI 全绿
3. 工具库（crc32 / hash / cpu_features / util）单测覆盖 ≥ 80%
4. `P0_infra_design.md` review 通过，锁定本阶段所有 schema 决策
5. README 与 ROADMAP 同步
```

改后：
```
1. GCC 15+ 与 Clang 20+ 双工具链 build 通过
2. ASAN / TSAN / UBSAN / Release 四档 × 双工具链本地 ctest 全绿
   （持续集成 CI 推迟到 P0 之外单独立项；M6-D1 锁定）
3. 工具库（util/ + common/）单测行覆盖 ≥ 80%
   （`scripts/run-coverage.sh --strict` 实证）
4. `P0M7_convergence_design.md` 审阅通过，锁定本阶段所有 schema 决策
5. README 与 ROADMAP 同步
6. `bench/baselines/p0_utilities.json` 归档（crc32 / hash 双工具链 Release 中位数）
```

### 8.3 `doc/P0/README.md` M7 条目

原文：
```
| M7 | P0 设计稿固化与状态同步 | `P0_infra_design.md`（阶段收敛文档） | 待撰写 |
```

改后：
```
| M7 | P0 设计稿固化与状态同步 | [P0M7_convergence_design.md](P0M7_convergence_design.md) | ✅ 已锁定 |
```

注：M7 自身状态用 `✅ 已锁定`（不带 "P0M7 收敛"——避免自指）。

### 8.4 根 `README.md` 表格 P0 行

原文：
```
| P0 | 基础设施 | — | 🚧 进行中 |
```

改后：
```
| P0 | 基础设施 | — | ✅ 完成 |
```

### 8.5 `ROADMAP.md` M7 段重写

按 M7-D3 + D5 决策完整重写 M7 段。新版要点（具体行号在落盘时定位）：

```markdown
- **M7：P0 设计稿固化与状态同步**
  - `doc/P0/P0M7_convergence_design.md` 完整撰写（薄索引形态——每章摘要 + 链回 P0M1–M6）
  - `doc/P1/README.md` / `doc/P2/README.md` 占位索引（含 ROADMAP 范围摘要 + 已知决策点候选 + 启动条件）
  - `scripts/run-bench.sh` 实装；工具库微基准基线归档到 `bench/baselines/p0_utilities.json`
    （crc32 / hash 双工具链 Release × 5 次重复 × 取中位数）
  - 评审残留 #9–#15 共 7 项 LOW 全部清场（P0 收敛点零债务）
  - 各 P0M1–M6 设计稿状态 → "✅ 已锁定（P0M7 收敛）"
  - `ROADMAP.md` P0 状态字串 → "已实施"；根 `README.md` 表格 P0 → "✅ 完成"
  - `doc/P0/README.md` 退出条件 6 条 + M7 条目按 M6 / M7 决策同步
  - 退出条件：见 `doc/P0/P0M7_convergence_design.md` §10
```

### 8.6 `ROADMAP.md` P0 阶段头部状态字串

ROADMAP §5 "P0 — 基础设施" 段（行号在落盘时定位）：把段头某处的"状态"字串（如有）→ "已实施"。

### 8.7 各 P0Mn 设计稿内引用 `P0_infra_design.md` 的位置

grep 残留 `P0_infra_design.md` / `p0_infra_design.md` —— 全部改为 `P0M7_convergence_design.md`。

---

## 9. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 薄索引收敛稿少了"全景叙事" | P1 新人需要看 7 份稿才能拼出 P0 全貌 | M7-D2 已锁定：未来全部完工后补一次厚整合稿 |
| 微基准基线机器相关 | 不同 CPU / 内核版本数值差几倍 | JSON schema 内嵌 env 段（kernel / CPU model / 特性）+ git_commit 完整记录 |
| 基线未来如何"对比" | 没有自动化对比脚本 | M7 仅做"建立基线"；对比工具留 P1+ 用得到时再立 |
| `--strict` 默认关 → 覆盖率漂移可能被忽略 | 开发者不主动跑 `--strict` 就不会被卡 | 退出条件 §10 第 3 / 4 步 owner 验收时手动 `--strict` 跑一次 |
| 评审残留 #10 `jq` 改造引入新依赖 | `setup-dev.sh` 若不补 `jq` 则 `--compiler=clang++` 覆盖率路径报错 | `run-coverage.sh` 自检段加 `jq` 依赖检查 + 提示 `sudo dnf install jq`；与 `gcovr` 同档处理 |
| P1 / P2 占位 README "已知决策点候选" 可能误导 | 候选清单不等于最终决策；P1 启动决策梳理时仍要重新识别 | README 内明确"候选仅作决策梳理提速参考，不预判答案" |
| 双工具链微基准数值差异 | GCC 与 Clang 在小数据量（64 字节）下可能差 2–3x | 基线归档双工具链各自值 + `notes` 段说明"差异属正常" |
| `.gitignore` 不在 M7 范围 | `bench/baselines/p0_utilities.json` 必须跟踪、不能被 `/build*/` 通配 ignore 误伤 | 临时区现行 `.gitignore` 的 `/build*/` 只匹配根下的 build 目录，`bench/baselines/` 不在排除范围；owner 落盘前自检即可 |

---

## 10. 退出条件与验证步骤

本里程碑退出条件六条，对应 M7-D1～D6 + owner 终审。

### 10.1 退出条件

1. **文档撰写**：`doc/P0/P0M7_convergence_design.md`（本文件，薄索引形态）+ `doc/P1/README.md`
   + `doc/P2/README.md`（中等深度占位索引）撰写完成。
2. **微基准基础设施 + 基线归档**：`scripts/run-bench.sh` 实装（`+x`、`bash -n` 通过、命令行
   按 §5.1）+ `bench/baselines/p0_utilities.json` 按 §5.2 schema 落入（双工具链 × 6 用例 × 5 次中位数）。
3. **评审残留全清**：#9–#15 共 7 项 LOW 修复全部落入。
4. **回归实证**：
   - `./scripts/run-tests.sh --asan && ./scripts/run-tests.sh --tsan && ./scripts/run-tests.sh --ubsan && ./scripts/run-tests.sh --release` —— **四档 PASS**（评审修复不退步，21 个测试用例全过）
   - `./scripts/run-coverage.sh --strict` —— 行覆盖率 **≥ 80%**（实测 98.4% 不退步）
   - `./scripts/run-bench.sh --baseline=bench/baselines/p0_utilities.json` —— **2/2 OK** +
     JSON 落地、`jq -e .` 校验通过
5. **状态同步全完**：
   - 各 P0M1–M6 设计稿 → "✅ 已锁定（P0M7 收敛）"（doc/P0/README.md 表格 + 各稿头部，若有）
   - `doc/P0/README.md` 退出条件 6 条 + M7 条目按 §8.2 / §8.3 更新
   - 根 `README.md` 表格 P0 → "✅ 完成"
   - `ROADMAP.md` M7 段按 §8.5 重写 + P0 状态字串 → "已实施" + 字面
     `doc/p1_core_design.md` / `doc/p2_api_freeze.md` → `doc/P1/README.md` / `doc/P2/README.md`
   - 全仓 `grep "P0_infra_design\|p0_infra_design"` 应为空（仅本设计稿历史段提及不算）
6. **owner 终审**：本设计稿 + 上述全部改动一次性审阅；通过即 P0 整体出口。

### 10.2 验证步骤（建议顺序）

1. 临时区先全部落入 → 跑 `run-tests.sh --asan && run-tests.sh --tsan && run-tests.sh --ubsan && run-tests.sh --release` + `run-coverage.sh --strict` + `run-bench.sh --baseline=...`
2. `jq -e . bench/baselines/p0_utilities.json` 校验 JSON 合法 + 字段齐全。
3. `grep -rn "P0_infra_design\|p0_infra_design" /home/cabe-workspace --include='*.md'` 验残留为空
   （本设计稿历史背景段除外）。
4. `grep -rn "🚧" /home/cabe-workspace/README.md /home/cabe-workspace/doc/P0/README.md` 验 P0
   不再有"进行中"字样。
5. `grep -rn "完成稿，待 owner 终审" /home/cabe-workspace/doc/P0/` 验 M1–M6 状态字串已收敛。
6. owner 通读 + 接受 → 落盘工作区 → P0 出口。

---

## 11. 对下游里程碑的接口承诺

| 下游 | 本里程碑提供的接入点 |
|---|---|
| **P1 启动** | `doc/P1/README.md` 已就位（启动条件 + 决策点候选）；`/grill-with-docs P1` 即可启动第一个里程碑 |
| **P2 启动** | `doc/P2/README.md` 已就位；P1 完成后启动 |
| **P1+ 业务模块复用 cabe 基础设施** | M1–M6 全部锁定结论（schema / 错误码 / hash / 日志 / 矩阵 / 覆盖率 / 微基准）+ 本设计稿 §4 作为统一入口 |
| **P1+ 微基准回归** | `scripts/run-bench.sh` 可一键复跑；`bench/baselines/p0_utilities.json` 作为 P0 基线参考（**不**强制后续阶段不退步——优化重心在 P3+） |
| **未来接持续集成** | `scripts/setup-dev.sh --ci` 与 `run-tests.sh` / `run-coverage.sh --strict` 三者在 CI 容器内可直接调用，无需重写（仓库托管确定后单独立项） |
| **未来厚整合稿** | 本设计稿 §4 各章已留好"链回上游"接口；厚整合稿替换薄索引、保留结构即可 |

---

## 12. 未来全部完工后的整合稿承诺（M7-D2 派生）

P0 收敛此刻采用薄索引形态。**未来全部完工后**会单独立项写一份厚整合稿，作用：
- 对外宣讲 / 用户文档的入口
- 把 schema / 错误码段位 / 术语 / CMake 选项 / 测试与微基准约定全部内联展开
- 替换本设计稿 §4 的薄索引部分；§3 决策 / §5 微基准方案 / §8 状态同步动作 等仍归档于本设计稿

立项时机：全部完工后最后一个里程碑（按 ROADMAP 当前规划为 P7 末或专门 release 里程碑）。

---

## 13. 与 ROADMAP 一致性核对

| ROADMAP M7 字面 | 本设计实现 | 状态 |
|---|---|---|
| `doc/p0_infra_design.md` 完整撰写 | `doc/P0/P0M7_convergence_design.md`（命名按 cabe 现行风格；M7-D1 锁定） | ✅ 已锁定（反向改 ROADMAP 字面） |
| 涵盖 schema / 错误码段位 / 术语 / CMake 选项 / 本地组合矩阵与覆盖率约定 / 测试·微基准约定 | §4 各章薄索引覆盖全部六项 | ✅ |
| `doc/p1_core_design.md` / `doc/p2_api_freeze.md` 骨架占位 | `doc/P1/README.md` / `doc/P2/README.md` 占位索引（M7-D5 决策；反向改 ROADMAP 命名字面） | ✅ |
| 工具库微基准基线归档 `bench/baselines/p0_utilities.json` | §5 全方案，含 `scripts/run-bench.sh` 实装（M7-D3） | ✅ |
| `ROADMAP.md` P0 状态字串更新为"已实施" | §8.5 / §8.6 | ✅ |
| `README.md` 表格 P0 → "✅ 完成" | §8.4 | ✅ |
| 退出条件：`P0_infra_design.md` review 通过；ROADMAP / README 同步落地 | §10 退出条件第 1 + 5 + 6 条覆盖；新增基线归档（第 2 条）与回归实证（第 4 条） | ✅ 扩展（ROADMAP 字面仅列两点，本设计扩展为六条更可验证） |

**有出入点**：
- ROADMAP 字面 `doc/p0_infra_design.md` / `doc/p1_core_design.md` / `doc/p2_api_freeze.md`
  统一改为 cabe 现行 `doc/P0/...` / `doc/P1/README.md` / `doc/P2/README.md` 风格——已在 §8.5
  落盘动作中列出。

---

**全文完。**
