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
| 状态 | **✅ 已锁定（P4M4 收敛）** |
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
| 性能基准归档 | **P6 起** | P4 不做性能基准；P6 建立首个正式 io_uring 历史锚点 |
| TSAN 注解（消除 io_uring 误报） | **不实施** | P7 最终用 sync + TSAN 覆盖后端无关并发结构，io_uring 专属路径用功能测试和审查验证 |
| 设备超级块（防止多设备顺序错乱） | **P5** | P5 做持久化和恢复，超级块与 recovery 流程配合（P5M1 兑现：头部 8K 双份 + 三设备配对校验，见 P5M1 稿；形态与 P4 预测的"第 0 块"不同——数据块编号不受影响） |
| 多设备参数 | **P7 已实现** | P7 脚本增加第二设备组参数；N≥8 真盘规模验证归 P11 |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 | 理由 |
|---|---|---|---|
| **P4M3-D1** | TSAN 兼容策略 | 维持互斥检查 + 文档说明 | ROADMAP 已豁免 io_uring + TSAN 组合；`run-tests.sh` 已拦截；sync 后端 + TSAN 可覆盖全部并发逻辑 |
| **P4M3-D2** | 测试设备传入方式 | `--device=PATH` 参数，脚本内部转为 `CABE_TEST_DEVICE` 环境变量 | 统一命令语义，避免一条命令中"环境变量 + 执行"的双重语义 |
| **P4M3-D3** | 性能基准 | P4 不归档；P6 起建立正式锚点 | 快速攒原型阶段不做；bench 框架代码保留不删 |
| **P4M3-D4** | 设备超级块 | 推到 P5 | P4 当时提出“第 0 块”方案；P5M1 最终改为设备头部双份 4K 超级块（共 8K），逻辑数据块仍从 0 编号 |

---

## 3. TSAN 兼容说明

### 3.1 不兼容的根本原因

io_uring 的提交队列和完成队列通过内存映射（mmap）与内核共享。用户态写入提交条目 → 内核读取并执行 → 内核写入完成条目 → 用户态读取结果。TSAN 只能追踪用户态的内存访问，看不到内核侧的读写操作，因此误报数据竞争。

### 3.2 当前处理

- `run-tests.sh`：`--backend=io_uring --tsan` 组合在参数解析阶段直接拒绝（已在 P4M1 前就预留）
- TSAN 测试只跑 sync 后端——sync 后端的 pread / pwrite 是系统调用，TSAN 可正确追踪
- sync 后端 + TSAN 足以覆盖 cabe 自身的全部并发逻辑（reactor、无锁队列、索引分区等在 Engine 层，与 I/O 后端无关）

### 3.3 未来演进

P7 最终没有给 io_uring 提交/等待路径增加 TSAN 注解：后端无关的 reactor 投递、唤醒和生命周期
由 sync + TSAN 覆盖，io_uring 专属路径通过功能测试和代码审查验证。P9 SPDK 延续这一证据边界：
共享并发结构继续由 sync + TSAN 覆盖，SPDK 专属代码通过显式设备测试、生命周期测试和代码审查验证。

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
改后（P6 起现行口径）：`./scripts/run-coverage.sh --backend=sync --strict --device=/dev/loop0`

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

> **后续实现校正（P5M1）**：本章以下内容保留 P4 当时的候选方案，不代表当前盘上布局。
> 最终实现使用设备头部主、备各 4K 的双份超级块；逻辑 block 0 映射到物理偏移 8K，
> `BlockAllocator` 不跳过任何逻辑块。权威布局见 [P5M1 设计稿](../P5/P5M1_super_block_design.md)。

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
3. **TSAN 兼容确认**：当前复跑使用 `run-tests.sh --backend=sync --tsan`；`--backend=io_uring --tsan` 被正确拒绝。
4. **现有测试不退步**：改用 `--device=` 参数后全部测试通过。

---

## 8. 对下游里程碑的接口承诺

| 下游 | 接入点 |
|---|---|
| **P4M4** | 收敛检查——确认 P4 全部里程碑完成 |
| **P5** | 设备超级块方案在 §6 已记录，P5 设计时参照实现 |
| **P7** | 最终采用 sync + TSAN 证明后端无关并发正确性，不给 io_uring 增加 TSAN 注解；多设备参数已落地 |
| **P11** | N≥8 真盘规模、带宽和隔离验证 |

---

**全文完。**
