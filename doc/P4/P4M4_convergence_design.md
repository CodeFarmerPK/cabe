# Cabe P4-M4 设计：阶段收敛与状态固化

> 本里程碑作为 P4 阶段收敛节点：撰写阶段收敛稿（薄索引形态），验证 P4 退出条件，
> 落下 P4.5 阶段占位索引，同步 ROADMAP 中 P4 期间产生的决策变更，并把各设计稿状态
> 推到锁定态。完成后 P4 整体出口。
>
> **本文为详细设计**。技术细节采用薄索引形态：每章只列锁定结论 + 链回 P4M1–M3
> 对应章节。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P4 / M4 |
| 状态 | **设计稿** |
| 上游依赖 | P4M1（io_uring 基础实现）、P4M2（预注册文件描述符）、P4M3（TSAN 兼容 + 部署文档 + 脚本改进） |
| 下游依赖本里程碑 | P4 阶段出口；P4.5 启动闸门 |
| 退出判定 | 见 §6 |

---

## 1. 目标与范围

### 1.1 目标

1. 把 P4 阶段全部技术结论凝练为单一收敛稿入口，便于 P4.5+ 读者跳到各上游设计稿。
2. 逐项验证 P4 退出条件（`doc/P4/README.md` §"P4 退出条件概要"）。
3. 落下 P4.5 阶段占位索引（`doc/P4.5/README.md`）。
4. 同步 ROADMAP 中 P4 期间产生的决策变更。
5. 状态同步：各设计稿 + P4 README + 根 README + ROADMAP。

### 1.2 交付范围

1. **`doc/P4/P4M4_convergence_design.md`**：本设计稿（薄索引形态）。
2. **`doc/P4.5/README.md`**：P4.5 阶段占位索引。
3. **状态同步**（详见 §4）。
4. **ROADMAP 更新**（详见 §5）。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| P4.5 里程碑划分与决策梳理 | **P4.5 启动时** | 收敛稿只放占位索引 |

---

## 2. 现状盘点

- **P4M1–M3 全部完成并已工作区落盘**：
  - P4M1（`8fe5fd6`）：`IoUringIoBackend` 基础实现——满足 IoBackend 的 5 个方法，最简提交/等待模型，CMake 分派生效
  - P4M2（`78f7a91`）：预注册文件描述符（`io_uring_register_files` + `IOSQE_FIXED_FILE`）
  - P4M3（暂存中）：TSAN 互斥确认 + 部署文档 + `--device=` 参数化 + 内核版本检查
- **全部测试通过**：sync 后端和 io_uring 后端四档全绿
- **P4 README / ROADMAP / 根 README 状态字段尚未更新**

---

## 3. 收敛技术索引（薄索引形态）

### 3.1 io_uring 基础实现（详 [P4M1_io_uring_basic_design.md](P4M1_io_uring_basic_design.md)）

- **IoUringIoBackend**：满足 IoBackend concept 的 5 个方法（Open / Close / BlockCount / Write / Read），`static_assert` 编译期验证。
- **liburing ≥ 2.9 硬性依赖**：CMake `pkg_check_modules` + `setup-dev.sh` 双重校验。
- **ring 生命周期**：与 SyncIoBackend 对称——Open 内初始化，Close 内销毁。
- **队列深度**：内部常量 64，P7 由系统自动推算，发版前不暴露到公开 API。
- **O_DIRECT**：保持，面向裸设备极致性能。
- **错误码**：统一 `err::kIoBase`，与 SyncIoBackend 一致。
- **决策锁定**：P4M1-D1~D5。

### 3.2 预注册文件描述符优化（详 [P4M2_io_uring_optimize_design.md](P4M2_io_uring_optimize_design.md)）

- **预注册文件描述符**：Open 时 `io_uring_register_files` 注册 fd，Write / Read 使用固定文件标志（`IOSQE_FIXED_FILE`），Close 时注销。减少每次 I/O 的内核文件描述符查找和引用计数开销。
- **预注册缓冲区推到 P8**：当前 IoBackend 接口只传内存地址不传索引号，P8 重新设计缓冲区管理时统一处理。
- **决策锁定**：P4M2-D1~D3。

### 3.3 TSAN 兼容 + 部署 + 脚本改进（详 [P4M3_tsan_deploy_design.md](P4M3_tsan_deploy_design.md)）

- **TSAN 兼容**：维持 `run-tests.sh` 已有的互斥检查（io_uring + TSAN 直接拒绝）。sync 后端 + TSAN 可覆盖 cabe 全部并发逻辑。TSAN 注解留 P7 评估。
- **部署文档**：`doc/P4/deploy.md`——系统要求、io_uring 启用检查、内存锁定限制、推荐设备路径、TSAN 说明、常见问题排查。
- **脚本改进**：`run-tests.sh` / `run-coverage.sh` 加 `--device=PATH` 参数，取代环境变量传入设备路径。
- **内核版本检查**：`setup-dev.sh` 加内核 ≥ 6.16 硬性校验。
- **设备超级块方案记录**：推到 P5——每个设备第 0 块写入身份标识，Open 时校验设备顺序。
- **决策锁定**：P4M3-D1~D4。

### 3.4 已锁定决策汇总

| 里程碑 | 编号 | 简述 |
|---|---|---|
| M1 | D1 | liburing 硬性系统依赖（≥ 2.9） |
| M1 | D2 | ring 在 Open 内初始化，与 SyncIoBackend 对称 |
| M1 | D3 | 队列深度 P4 固定 64，P7 自动推算 |
| M1 | D4 | 保持 O_DIRECT |
| M1 | D5 | 错误码统一 `kIoBase` |
| M2 | D1 | 只做预注册文件描述符，预注册缓冲区推到 P8 |
| M2 | D2 | Open 内注册，Close 时注销 |
| M2 | D3 | 注册失败统一返回错误 |
| M3 | D1 | TSAN 维持互斥检查 |
| M3 | D2 | 测试设备 `--device=` 参数化 |
| M3 | D3 | 性能基准发版后再补 |
| M3 | D4 | 设备超级块推到 P5 |

---

## 4. 状态同步动作清单

### 4.1 各 P4M1–M3 设计稿状态字串

| 文件 | 当前状态 | 改后状态 |
|---|---|---|
| `P4M1_io_uring_basic_design.md` | 设计稿 | ✅ 已锁定（P4M4 收敛） |
| `P4M2_io_uring_optimize_design.md` | 设计稿 | ✅ 已锁定（P4M4 收敛） |
| `P4M3_tsan_deploy_design.md` | 设计稿 | ✅ 已锁定（P4M4 收敛） |

### 4.2 `doc/P4/README.md` 更新

**阶段状态**：
```diff
- 🚧 **未启动**（P3 全部完成；待 owner 确认启动）
+ ✅ **已完成**（P4M4 收敛通过）
```

**里程碑文档清单状态列**：M1–M3 → "✅ 已锁定（P4M4 收敛）"；M4 → "✅ 已锁定"。

### 4.3 根 `README.md` 表格 P4 行

```diff
- | P4 | io_uring 后端 | ⏳ |
+ | P4 | io_uring 后端 | ✅ 完成 |
```

### 4.4 `ROADMAP.md` P4 状态字串

```diff
  ### P4 — io_uring 后端
+
+ **状态**：✅ 已实施（P4M4 收敛通过；详见 [doc/P4/P4M4_convergence_design.md](doc/P4/P4M4_convergence_design.md)）
```

---

## 5. ROADMAP 决策变更同步

P4 期间产生的三项决策变更需要同步到 ROADMAP：

### 5.1 预注册缓冲区从 P4 移到 P8

ROADMAP P4 范围中原文提到"registered buffer 注册"——实际实现中只做了预注册文件描述符，预注册缓冲区推到 P8（P4M2-D1 决策）。ROADMAP P4 段需更新措辞。

### 5.2 性能基准发版后再补

ROADMAP 阶段间衔接约定中要求"bench 归档到 `bench/baselines/pN_xxx.json`"。P4 决策不做性能基准归档（P4M3-D3），bench 框架保留但不强制跑。需在 P4 段注明。

### 5.3 设备超级块加入 P5

P5 范围中原文未提及设备超级块。P4M3-D4 决策将其加入 P5——每个设备第 0 块写入身份标识（引擎 UUID + 设备编号），Open 时校验顺序。需在 ROADMAP P5 段补充。同时 D2（"数据设备只放原始 value 字节"）和 D3（"数据设备不存任何元数据"）需加注"第 0 块为超级块例外"。

---

## 6. 退出条件

### 6.1 退出条件（5 条）

1. **文档撰写**：`doc/P4/P4M4_convergence_design.md`（本文件）+ `doc/P4.5/README.md`（占位索引）撰写完成。
2. **P4 退出条件逐项验证**（对应 `doc/P4/README.md` §"P4 退出条件概要"）：
   - ✅ `IoUringIoBackend` 实装 + `static_assert` 通过
   - ✅ CMake `-DCABE_IO_BACKEND=io_uring` 编译、链接、测试全绿
   - ✅ 预注册文件描述符启用
   - ✅ TSAN 兼容方案落地（互斥检查 + 文档说明）
   - ✅ 部署文档就位
   - ✅ 测试脚本支持 `--device=` 参数
   - ✅ P4M4 收敛稿审阅通过 + ROADMAP / README 状态同步
3. **回归实证**：
   - `run-tests.sh --backend=io_uring --release --device=/dev/loop0` 全绿
   - `run-tests.sh --release --device=/dev/loop0`（sync 不退步）全绿
   - `run-coverage.sh --strict`（默认 sync 后端）覆盖率 ≥ 80%
4. **状态同步全完**：设计稿 / P4 README / 根 README / ROADMAP 全部更新。
5. **owner 终审**：本设计稿 + 全部改动审阅通过；通过即 P4 整体出口。

### 6.2 验证命令

```bash
# io_uring 后端测试
./scripts/run-tests.sh --backend=io_uring --release --device=/dev/loop0
./scripts/run-tests.sh --backend=io_uring --asan --device=/dev/loop0
./scripts/run-tests.sh --backend=io_uring --ubsan --device=/dev/loop0

# sync 后端不退步
./scripts/run-tests.sh --release --device=/dev/loop0

# 覆盖率
./scripts/run-coverage.sh --strict --device=/dev/loop0
```

---

## 7. 对下游里程碑的接口承诺

| 下游 | 接入点 |
|---|---|
| **P4.5 启动** | `doc/P4.5/README.md` 已就位 |
| **P5** | 设备超级块方案在 P4M3 §6 已记录，P5 设计时参照实现；ROADMAP 已同步 |
| **P7** | 队列深度自动推算 + TSAN 注解评估 |
| **P8** | 预注册缓冲区 + 缓冲区管理重新设计 |

---

**全文完。**
