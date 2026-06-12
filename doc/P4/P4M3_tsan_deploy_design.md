# Cabe P4-M3 设计：TSAN 兼容 + 部署文档 + 脚本改进

> 本里程碑完成 P4 的工程质量收尾：确认 TSAN 与 io_uring 的兼容策略（维持互斥检查），
> 编写 io_uring 后端的部署文档，并将测试脚本的设备指定方式从环境变量改为 `--device=`
> 参数化传入。
>
> **本文为详细设计**。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P4 / M3 |
| 状态 | **设计稿** |
| 上游依赖 | P4M2（预注册文件描述符优化） |
| 下游依赖本里程碑 | P4M4（收敛） |
| 退出判定 | 见 §7 |

---

## 1. 目标与范围

### 1.1 目标

1. 确认 TSAN 与 io_uring 的兼容策略——维持 `run-tests.sh` 已有的互斥检查，部署文档中说明原因。
2. 编写 io_uring 后端的部署文档——系统要求、权限配置、常见问题。
3. 将 `run-tests.sh` / `run-coverage.sh` 的设备指定方式从环境变量（`CABE_TEST_DEVICE=...`）改为脚本参数（`--device=...`）。
4. 在设计文档中记录"设备超级块方案推到 P5"的决策。

### 1.2 交付范围

1. **`scripts/run-tests.sh`**（修改）：加 `--device=PATH` 参数。
2. **`scripts/run-coverage.sh`**（修改）：加 `--device=PATH` 参数。
3. **`doc/P4/deploy.md`**（新建）：io_uring 后端部署文档。
4. **`doc/P4/P4M3_tsan_deploy_design.md`**：本设计稿。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| 性能基准归档 | **发版后** | 快速攒原型阶段不做性能基准 |
| TSAN 注解（消除 io_uring 误报） | **P7** | P4 单线程无并发，TSAN 无实际价值；P7 多线程时再评估 |
| 设备超级块（防止多设备顺序错乱） | **P5** | P5 做持久化和恢复，超级块与 recovery 流程配合（P5M1 兑现：头部 8K 双份 + 三设备配对校验，见 P5M1 稿；形态与 P4 预测的"第 0 块"不同——数据块编号不受影响） |
| 多设备 `--device=` 支持 | **P7/P11** | 当前单设备，多设备参数格式待定 |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 | 理由 |
|---|---|---|---|
| **P4M3-D1** | TSAN 兼容策略 | 维持互斥检查 + 文档说明 | ROADMAP 已豁免 io_uring + TSAN 组合；`run-tests.sh` 已拦截；sync 后端 + TSAN 可覆盖全部并发逻辑 |
| **P4M3-D2** | 测试设备传入方式 | `--device=PATH` 参数，脚本内部转为 `CABE_TEST_DEVICE` 环境变量 | 统一命令语义，避免一条命令中"环境变量 + 执行"的双重语义 |
| **P4M3-D3** | 性能基准 | 发版后再补 | 快速攒原型阶段不做；bench 框架代码保留不删 |
| **P4M3-D4** | 设备超级块 | 推到 P5 | 每个设备第 0 块写入标识（设备编号 + 引擎全局 UUID），Open 时校验顺序；P5 做持久化时一并处理 |

---

## 3. TSAN 兼容说明

### 3.1 不兼容的根本原因

io_uring 的提交队列和完成队列通过内存映射（mmap）与内核共享。用户态写入提交条目 → 内核读取并执行 → 内核写入完成条目 → 用户态读取结果。TSAN 只能追踪用户态的内存访问，看不到内核侧的读写操作，因此误报数据竞争。

### 3.2 当前处理

- `run-tests.sh`：`--backend=io_uring --tsan` 组合在参数解析阶段直接拒绝（已在 P4M1 前就预留）
- TSAN 测试只跑 sync 后端——sync 后端的 pread / pwrite 是系统调用，TSAN 可正确追踪
- sync 后端 + TSAN 足以覆盖 cabe 自身的全部并发逻辑（reactor、无锁队列、索引分区等在 Engine 层，与 I/O 后端无关）

### 3.3 未来演进

P7 多线程阶段如果需要在 io_uring 后端下做 TSAN 验证，可在 io_uring 的提交/等待路径中插入 TSAN 同步注解（`__tsan_acquire` / `__tsan_release`）。SPDK 后端（P10）大概率始终豁免——用户态直接操作硬件寄存器，TSAN 无法追踪。

---

## 4. 脚本改进：`--device=` 参数化

### 4.1 run-tests.sh

用法新增：
```
设备:
  --device=PATH       测试设备路径（如 /dev/loop0）
                      不传则跳过需要设备的测试
```

内部实现：解析 `--device=` 参数后，在 ctest 运行前设置 `CABE_TEST_DEVICE` 环境变量。测试代码（`std::getenv("CABE_TEST_DEVICE")`）无需修改。

改前：`CABE_TEST_DEVICE=/dev/loop0 ./scripts/run-tests.sh --backend=io_uring --release`
改后：`./scripts/run-tests.sh --backend=io_uring --release --device=/dev/loop0`

### 4.2 run-coverage.sh

同理加 `--device=PATH` 参数。

改前：`CABE_TEST_DEVICE=/dev/loop0 ./scripts/run-coverage.sh --strict`
改后：`./scripts/run-coverage.sh --strict --device=/dev/loop0`

---

## 5. 部署文档大纲

新建 `doc/P4/deploy.md`，内容：

1. **系统要求**：Linux 内核 ≥ 6.16、liburing ≥ 2.9、Fedora 43+
2. **io_uring 启用检查**：`cat /proc/sys/kernel/io_uring_disabled`（0 = 启用，1 = 需要 CAP_SYS_ADMIN，2 = 完全禁用）
3. **内存锁定限制**：`ulimit -l` 查看当前限制；io_uring 的 ring 和预注册文件描述符需要锁定内存；不够时调整 `/etc/security/limits.conf`
4. **推荐设备路径**：使用持久路径（`/dev/disk/by-id/`）而非易变路径（`/dev/nvmeXnY`），避免重启后盘符漂移
5. **TSAN 说明**：io_uring 后端不支持 TSAN 测试，sync 后端 + TSAN 可覆盖全部并发逻辑
6. **常见问题**：CMake 报 liburing 找不到（`setup-dev.sh` 安装）、io_uring 被 sysctl 禁用、内存锁定不足

---

## 6. 设备超级块方案记录（推到 P5）

### 6.1 问题

cabe 的路由公式 `hash(key) % N` 依赖设备顺序。如果重启后传入 `Options.devices` 的顺序与首次 Open 不同，所有 key 指向错误的设备，数据等于全部损坏。

### 6.2 方案

每个设备的第 0 块作为超级块，写入：
- 引擎全局 UUID（首次 Open 时生成）
- 设备编号（`device_id`，在 `devices` 数组中的位置）
- 创建时间戳

后续 Open 时读取每个设备的超级块，校验 UUID 一致 + 设备编号与传入顺序匹配。校验失败直接拒绝打开。

### 6.3 影响

- 每个设备牺牲第 0 块（1 MiB）存放超级块
- FreeList 从块 1 开始分配
- ROADMAP D2（"数据设备只放原始 value 字节"）和 D3（"数据设备不存任何元数据"）需放宽——超级块只占第 0 块，数据区域（块 1 ~ N）仍为纯 value 字节

### 6.4 落点

P5（WAL + 崩溃恢复）阶段一并实现——超级块校验与 recovery 流程天然配合。

---

## 7. 退出条件

1. **脚本改进完成**：`run-tests.sh` / `run-coverage.sh` 支持 `--device=PATH` 参数；旧的环境变量方式仍兼容。
2. **部署文档就位**：`doc/P4/deploy.md` 覆盖系统要求、权限配置、常见问题。
3. **TSAN 兼容确认**：`run-tests.sh --tsan` 全绿（sync 后端）；`--backend=io_uring --tsan` 被正确拒绝。
4. **现有测试不退步**：改用 `--device=` 参数后全部测试通过。

---

## 8. 对下游里程碑的接口承诺

| 下游 | 接入点 |
|---|---|
| **P4M4** | 收敛检查——确认 P4 全部里程碑完成 |
| **P5** | 设备超级块方案在 §6 已记录，P5 设计时参照实现 |
| **P7** | TSAN 注解在 §3.3 已记录，P7 多线程阶段按需评估 |
| **P7/P11** | 多设备 `--device=` 参数格式待定 |

---

**全文完。**
