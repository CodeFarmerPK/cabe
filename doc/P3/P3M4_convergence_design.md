# Cabe P3-M4 设计：阶段收敛与状态固化

> 本里程碑作为 P3 阶段收敛节点：撰写阶段收敛稿（薄索引形态），验证 P3 退出条件，
> 落下 P4 阶段占位索引，并把 `ROADMAP.md`、根 `README.md`、`doc/P3/README.md` 与
> 各 P3M1–M3 设计稿的状态字串一并推到锁定态。完成后 P3 整体出口。
>
> **本文为详细设计**。技术细节采用薄索引形态：每章只列锁定结论 + 链回 P3M1–M3
> 对应章节，不复制接口定义（详见各上游设计稿）。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P3 / M4 |
| 状态 | **✅ 已锁定（P3M4 收敛）** |
| 上游依赖 | P3M1（IoBackend + SyncIoBackend）、P3M2（MetaIndexBackend + HashMetaIndex）、P3M3（Engine 切换 + CMake 分派） |
| 下游依赖本里程碑 | P3 阶段出口；P4 启动闸门 |
| 退出判定 | 见 §6 |

---

## 1. 目标与范围

### 1.1 目标

1. 把 P3 阶段全部技术结论凝练为单一收敛稿入口（`P3M4_convergence_design.md`），
   便于 P4+ 读者从这里跳到各上游详细稿。
2. 逐项验证 P3 退出条件（`doc/P3/README.md` §"P3 退出条件概要"列出的 5 条）。
3. 落下 P4 阶段占位索引（`doc/P4/README.md`），明确启动条件。
4. 把 `ROADMAP.md`、根 `README.md`、`doc/P3/README.md`、各 P3M1–M3 设计稿的
   状态字串一并推到锁定态，做到全仓状态一致。

### 1.2 交付范围

1. **`doc/P3/P3M4_convergence_design.md`**：本设计稿（薄索引形态）。
2. **`doc/P4/README.md`**：P4 阶段占位索引。
3. **状态同步**（详见 §4）：
   - 各 P3M1–M3 设计稿状态 → "✅ 已锁定（P3M4 收敛）"
   - `doc/P3/README.md` 状态与里程碑清单更新
   - 根 `README.md` 表格 P3 行 → "✅ 完成"
   - `ROADMAP.md` P3 状态字串 → "✅ 已实施"

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| 性能基线归档 | **不需要** | P3 功能等价于 P2，无新性能特性；P4 io_uring 才有性能对比意义 |
| 评审残留清理 | **不需要** | P3M1–M3 无遗留评审问题 |
| P4 里程碑划分与决策梳理 | **P4 启动时** | M4 只放占位索引；待触发 `/grill-with-docs P4M1` 时正式启动 |

---

## 2. 现状盘点

- **P3M1–M3 全部完成并已工作区落盘**：
  - P3M1（`6d05e0e` 之前）：`io/io_backend.h`（5 方法 C++20 concept）+ `io/sync/sync_io_backend.*`（SyncIoBackend 实装）+ 10 个单元测试
  - P3M2（`a78f484`）：`index/meta_index.h`（7 方法 C++20 concept）+ `index/hash/hash_meta_index.*`（HashMetaIndex 实装）+ 10 个契约测试
  - P3M3（`6d05e0e`）：`engine/backend_config.h`（配置头）+ DeviceContext / Engine 改造 + CMake 分派生效 + 旧代码清理（6 个文件删除）
- **全部测试通过**：P3M3 提交后四档（release / asan / tsan / ubsan）全绿
- **P3 README / ROADMAP / 根 README 状态字段尚未更新**：
  - `doc/P3/README.md` 状态仍为 "🚧 已启动"；里程碑清单全标"待设计"
  - 根 `README.md` P3 行为 "⏳"
  - 各 P3M1–M3 设计稿状态为"完成稿（待 owner 终审）"或"设计稿"
- **`doc/P4/` 目录不存在**：本里程碑新建

---

## 3. 收敛技术索引（薄索引形态）

### 3.1 IoBackend 抽象层（详 [P3M1_io_backend_design.md](P3M1_io_backend_design.md)）

- **IoBackend C++20 concept**：5 个同步方法——`Open(path)` / `Close()` / `BlockCount()` / `Write(block_idx, buf)` / `Read(block_idx, buf)`，全部返回 `int32_t` 错误码（`BlockCount` 例外，返回 `uint64_t`）。
- **SyncIoBackend**：包装 `::open(O_DIRECT)` + `ioctl(BLKGETSIZE64)` + `pwrite` / `pread` + `::close`，管理完整设备生命周期。
- **目录结构**：`io/io_backend.h`（接口）+ `io/sync/sync_io_backend.*`（实现）——P4 加 `io/uring/`，P10 加 `io/spdk/`。
- **编译期验证**：`static_assert(IoBackend<SyncIoBackend>)` 在头文件中。
- **决策锁定**：P3M1-D1（完整生命周期方法）、P3M1-D2（空构造 + Open）、P3M1-D3（子目录分层）。

### 3.2 MetaIndex 抽象层（详 [P3M2_meta_index_design.md](P3M2_meta_index_design.md)）

- **MetaIndexBackend C++20 concept**：7 个方法——`Insert` / `Lookup` / `Delete` / `Size` / `Contains` / `ForEach` / `WriteSnapshot` / `LoadSnapshot`（后两个空壳，P5 实装）。
- **HashMetaIndex**：包装 `unordered_map<string, ValueMeta>`；`ForEach` 已实装遍历逻辑，`WriteSnapshot` / `LoadSnapshot` 返回 `kEngineNotImplemented`。
- **MetaIndexVisitor**：`std::function<void(string_view, const ValueMeta&)>`。
- **目录结构**：`index/meta_index.h`（接口）+ `index/hash/hash_meta_index.*`（实现）——P9 加 `index/bplustree/`。
- **契约测试**：`TYPED_TEST` 形态——P9 加 B+ 树只需改 `Types<>` 列表。
- **决策锁定**：P3M2-D1（与 io/ 对称的目录布局）、P3M2-D2（`TYPED_TEST` 契约测试）。

### 3.3 Engine 切换 + CMake 分派（详 [P3M3_engine_switch_design.md](P3M3_engine_switch_design.md)）

- **配置头**：`engine/backend_config.h`——根据 CMake 编译定义（`CABE_USE_IO_SYNC` / `CABE_USE_META_HASHMAP`）选择具体类型，using 别名 `IoBackendImpl` / `MetaIndexImpl`，末尾 `static_assert` 验证满足 concept。
- **DeviceContext 改造**：`int fd` → `IoBackendImpl io`；`MetaIndex` → `MetaIndexImpl`。
- **Engine 改造**：`::open` / `WriteBlock` / `ReadBlock` / `::close` → `dc.io.Open` / `dc.io.Write` / `dc.io.Read` / `dc.io.Close`；系统头文件（`fcntl.h` / `linux/fs.h` / `sys/ioctl.h` / `unistd.h`）从 engine.cpp 移除。
- **CMake 分派**：`engine/CMakeLists.txt` 根据 `CABE_IO_BACKEND` / `CABE_META_INDEX` 传递 PUBLIC 编译定义；根 CMakeLists.txt 移除了 P3 前的占位提示。
- **旧代码清理**：删除 `engine/io.*`、`engine/meta_index.*`、`test/engine/io_test.cpp`、`test/engine/meta_index_test.cpp`（共 6 个文件），已被 P3M1/M2 的实现和测试完整覆盖。
- **决策锁定**：P3M3-D1（编译期切换，方案 C）、P3M3-D2（Engine 不变模板类）、P3M3-D3（旧代码删除）。

### 3.4 已锁定决策汇总

| 阶段级 | 里程碑级 | 简述 |
|---|---|---|
| P3-D1 | — | 同步接口，无 poll 模型 |
| P3-D2 | — | BufferHandle 推到 P8 |
| P3-D3 | — | 7 个方法（含空壳） |
| P3-D4 | — | 不做伪 SPDK / Mock |
| P3-D5 | — | 4 个里程碑串行 |
| — | P3M1-D1~D3 | 完整生命周期 / 空构造 + Open / 子目录分层 |
| — | P3M2-D1~D2 | 与 io/ 对称布局 / `TYPED_TEST` 契约测试 |
| — | P3M3-D1~D3 | 编译期切换方案 C / Engine 非模板 / 旧代码删除 |

---

## 4. 状态同步动作清单

### 4.1 各 P3M1–M3 设计稿状态字串

各设计稿 §0 元信息表格"状态"行：

| 文件 | 当前状态 | 改后状态 |
|---|---|---|
| `P3M1_io_backend_design.md` | 完成稿（待 owner 终审） | ✅ 已锁定（P3M4 收敛） |
| `P3M2_meta_index_design.md` | 完成稿（待 owner 终审） | ✅ 已锁定（P3M4 收敛） |
| `P3M3_engine_switch_design.md` | 设计稿 | ✅ 已锁定（P3M4 收敛） |

### 4.2 `doc/P3/README.md` 更新

**阶段状态**：

```diff
- 🚧 **已启动**（P2 全部完成；P3-D1~D5 决策已锁定）
+ ✅ **已完成**（P3M4 收敛通过）
```

**里程碑文档清单状态列**：

| 里程碑 | 当前状态 | 改后状态 |
|---|---|---|
| M1 | 待设计 | ✅ 已锁定（P3M4 收敛） |
| M2 | 待设计 | ✅ 已锁定（P3M4 收敛） |
| M3 | 待设计 | ✅ 已锁定（P3M4 收敛） |
| M4 | 待设计 | ✅ 已锁定 |

### 4.3 根 `README.md` 表格 P3 行

```diff
- | P3 | IoBackend + MetaIndex 抽象 | ⏳ |
+ | P3 | IoBackend + MetaIndex 抽象 | ✅ 完成 |
```

### 4.4 `ROADMAP.md` P3 状态字串

P3 段头部加状态标注：

```diff
  ### P3 — IoBackend 与 MetaIndex 抽象层
+
+ **状态**：✅ 已实施（P3M4 收敛通过；详见 [doc/P3/P3M4_convergence_design.md](doc/P3/P3M4_convergence_design.md)）
```

### 4.5 P4 占位索引

新建 `doc/P4/README.md`，内容大纲：

1. **状态**：🚧 未启动（待 P3M4 收敛通过 + owner 确认启动）。
2. **阶段目标**（摘 ROADMAP "P4 — io_uring 后端"）：io_uring 后端，启用 registered buffers + FIXED ops + register_files。
3. **范围摘要**：`IoUringIoBackend` 完整实现、liburing ≥ 2.9 接入、per-(device, reactor) 一个独立 ring、submit / wait 模型。
4. **里程碑文档清单**（占位）：待决策梳理划分。
5. **启动条件**：P3M4 收敛稿审阅通过 + owner 确认 + 用 `/grill-with-docs P4M1` 开第一个里程碑。
6. **已知决策点候选**：
   - io_uring ring 大小与队列深度
   - registered buffers 与 BufferPool 的交互
   - submit 批处理策略（逐个 submit 还是批量）
   - TSAN 与 io_uring 的兼容性处理
   - liburing 接入方式（系统库优先 vs 内嵌）

---

## 5. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| P3 未做性能基线归档 | P4 io_uring 没有 P3 sync 基线做对照 | P3 功能等价于 P2；P4 设计稿中可先跑 sync 基线再对比 io_uring |
| 编译定义 PUBLIC 传播 | `CABE_USE_IO_SYNC` 等定义传播到所有链接 `cabe_engine` 的目标 | cabe 是独立项目，不作为第三方库发布 |
| 旧代码已删除，无法对比 | `engine/io.*` / `engine/meta_index.*` 已删 | git 历史可追溯 |

---

## 6. 退出条件

### 6.1 退出条件（5 条）

1. **文档撰写**：`doc/P3/P3M4_convergence_design.md`（本文件）+ `doc/P4/README.md`（占位索引）撰写完成。
2. **P3 退出条件逐项验证**（对应 `doc/P3/README.md` §"P3 退出条件概要"）：
   - ✅ IoBackend concept 定义 + SyncIoBackend 实装 + 10 个单元测试全绿
   - ✅ MetaIndex concept 定义 + HashMetaIndex 实装 + 10 个契约测试全绿
   - ✅ Engine 通过 concept 调用——测试不退步 + 覆盖率 ≥ 80%
   - ✅ CMake `CABE_IO_BACKEND=sync` / `CABE_META_INDEX=hashmap` 编译期分派生效；设置其他值 `FATAL_ERROR`
   - ✅ P3M4 收敛稿审阅通过 + ROADMAP / README 状态同步
3. **回归实证**：
   - `run-tests.sh --release` / `--asan` / `--tsan` / `--ubsan` 四档全绿
   - `run-coverage.sh --strict` 覆盖率 ≥ 80%
4. **状态同步全完**：
   - 各 P3M1–M3 设计稿 → "✅ 已锁定（P3M4 收敛）"
   - `doc/P3/README.md` → "✅ 已完成" + 里程碑清单更新
   - 根 `README.md` P3 行 → "✅ 完成"
   - `ROADMAP.md` P3 状态 → "✅ 已实施"
5. **owner 终审**：本设计稿 + 上述全部改动审阅通过；通过即 P3 整体出口。

### 6.2 验证命令

```bash
# 四档测试
scripts/run-tests.sh --release
scripts/run-tests.sh --asan
scripts/run-tests.sh --tsan
scripts/run-tests.sh --ubsan

# 覆盖率
scripts/run-coverage.sh --strict

# CMake 分派验证
cmake -S . -B /tmp/cabe-uring-check -DCABE_IO_BACKEND=io_uring 2>&1 | grep "FATAL_ERROR"
cmake -S . -B /tmp/cabe-btree-check -DCABE_META_INDEX=bplustree 2>&1 | grep "FATAL_ERROR"

# 状态同步验证
grep -rn "🚧\|待设计\|待 owner 终审\|⏳" doc/P3/ README.md ROADMAP.md | grep -i "P3"
```

---

## 7. 对下游里程碑的接口承诺

| 下游 | 本里程碑提供的接入点 |
|---|---|
| **P4 启动** | `doc/P4/README.md` 已就位（启动条件 + 决策点候选）；`/grill-with-docs P4M1` 即可启动 |
| **P4 io_uring 后端** | `engine/backend_config.h` 加 `#elif CABE_USE_IO_URING` 分支 + `engine/CMakeLists.txt` 加 `elseif` 即可接入 |
| **P9 B+ 树索引** | `engine/backend_config.h` 加 `#elif CABE_USE_META_BPLUSTREE` 分支 + 契约测试 `Types<>` 加类型即可接入 |
| **P4+ 模块复用 P3 抽象层** | IoBackend / MetaIndexBackend concept 作为稳定接口；新后端只需实现 concept 并 `static_assert` 验证 |

---

**全文完。**
