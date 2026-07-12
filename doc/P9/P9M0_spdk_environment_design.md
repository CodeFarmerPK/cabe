# Cabe P9-M0 设计：SPDK 环境与路线基线

> 本里程碑是 P9 的启动基线：将 Cabe 从“系统中不存在 SPDK”的干净状态推进到
> “仓库内固定版本 SPDK 子模块 + Cabe 包装脚本 + 显式 NVMe 设备接管”的工程形态。
> P9M0 同时负责把 P9 最新路线向前同步到已完成文档，消除旧路线中
> “P9=B+树、P10=SPDK”的过期描述。
>
> **本文为 P9M0 详细设计**，汇总 P9M0-D1 ~ P9M0-D18 的全部裁决。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P9 / M0 |
| 状态 | ✅ 已实现；高风险系统操作保留为手动执行 |
| 上游依赖 | P1 ~ P8 已完成；P9 总体计划见 `doc/P9/README.md` |
| 下游依赖本里程碑 | P9M1 构建接入与配置模型；P9M2 定向探测工具；P9M3 qpair / DMA / 读写验证 |
| 退出判定 | 见 §13 |

---

## 1. 目标与范围

### 1.1 目标

1. **固定 P9 最新路线**：P9 正式定位为 SPDK NVMe API 后端接入；生产级无锁内存 B+树索引后移到 P10。
2. **引入 SPDK 子模块**：在 `third_party/spdk` 下接入 SPDK 子模块，固定到 `v26.01` 对应提交。
3. **建立 SPDK 环境入口**：新增 `scripts/setup-spdk.sh`，负责子模块初始化、依赖安装、编译、检查、大页内存显式配置。
4. **建立 NVMe 设备管理入口**：新增 `scripts/spdk-device.sh`，负责只读状态展示、显式接管和显式释放指定 BDF。
5. **固定安全边界**：设备操作必须单 BDF、强确认、无批量、无自动选择、无强制绕过、无驱动自动降级。
6. **记录开发机设备规划快照**：在文档中记录当前开发环境的 NVMe 规划，但脚本和代码不硬编码 BDF。
7. **全量文档路线对齐**：以 P9 最新设计为准，向前同步 P0 ~ P8 已完成文档中的后续路线引用。

### 1.2 交付范围

| 交付物 | 类别 | 说明 |
|---|---|---|
| `doc/P9/P9M0_spdk_environment_design.md` | 文档 | 本文档 |
| `third_party/spdk` | 子模块 | 固定 SPDK `v26.01` 对应提交 |
| `scripts/setup-spdk.sh` | 脚本 | SPDK 子模块、依赖、编译、检查和大页内存显式配置 |
| `scripts/spdk-device.sh` | 脚本 | NVMe 设备只读状态、显式接管、显式释放 |
| `README.md` / `ROADMAP.md` / `CONTEXT.md` | 文档同步 | 路线级信息对齐 P9 最新设计 |
| `doc/P0` ~ `doc/P8` | 文档同步 | 已完成文档中后续路线、阶段编号、债务流向对齐 |
| `doc/P9/README.md` | 文档同步 | P9M0 范围和脚本边界对齐本文 |

### 1.3 明确不做

| 不做项 | 归属 |
|---|---|
| Cabe 主 CMake 查找和链接 SPDK | P9M1 |
| `Options::spdk_devices` 类型化配置 | P9M1 |
| SPDK 专属错误码段 | P9M1 |
| Cabe 自有 SPDK probe 工具 | P9M2 |
| namespace 只读枚举工具 | P9M2 |
| qpair 创建、completion 轮询 | P9M3 |
| SPDK DMA 内存分配验证 | P9M3 |
| 1 MiB 读写和 CRC 校验 | P9M3 |
| `SpdkNvmeDevice` 薄封装 | P9M4 |
| `SpdkIoBackend` | P9M6 |
| SPDK value 零拷贝路径 | P9M7 |
| WAL / snapshot SPDK 接入 | P9M9 / P9M11 |
| SPDK bench 归档 | P9M14 |

---

## 2. 决策汇总

| 编号 | 决策 | 结论 |
|---|---|---|
| **P9M0-D1** | 实施边界 | M0 做文档同步和非侵入式环境脚本基线；不接入 Cabe 主 CMake，不修改 Engine 主路径。 |
| **P9M0-D2** | 设计权威 | 以已确认的最新设计为准；新旧设计冲突时，新设计优先，并由当前里程碑同步回文档。 |
| **P9M0-D3** | 阶段重排 | P9=SPDK，P10=B+树，P11=多 NVMe 规模化与真盘验证，P12=可观测性与运维工具。 |
| **P9M0-D4** | SPDK 来源 | P9M0 起只使用 `third_party/spdk` 子模块；不使用 `/home/pk/spdk`，不提供 `CABE_SPDK_ROOT`。 |
| **P9M0-D5** | 版本锁定 | 子模块固定到 SPDK `v26.01` 对应提交；脚本校验 commit 和 `VERSION=26.01.0`。 |
| **P9M0-D6** | 脚本职责 | `setup-dev.sh` 只管 Cabe 基础依赖；SPDK 由 `setup-spdk.sh` 独立管理。 |
| **P9M0-D7** | `setup-spdk.sh` 命令 | 支持 `init/deps/build/check/all/clean`，并采用严格 fail-fast；不修复、不静默处理。 |
| **P9M0-D8** | `status` 边界 | `spdk-device.sh status` 是只读诊断入口，展示 NVMe 状态和接管安全判断。 |
| **P9M0-D9** | `bind/unbind` 安全 | 单 BDF、强确认、无批量、无自动选择、无 `--force`；Cabe 做 holder、挂载点、系统盘、目标驱动和 IOMMU 前置检查，并在底层动作后验证驱动与块设备状态。 |
| **P9M0-D10** | 大页内存 | `check` 只读检查；`hugepage --mem-mib=1024 --confirm` 显式配置；`all` 不自动配置。 |
| **P9M0-D11** | 用户态驱动 | 默认 `vfio-pci`，要求目标设备存在 IOMMU group；显式允许 `uio_pci_generic`，禁止自动降级，不支持 `igb_uio`。 |
| **P9M0-D12** | 开发机设备规划 | 文档记录当前开发机快照；脚本和代码不硬编码 BDF、用途或容量。 |
| **P9M0-D13** | 验证边界 | M0 验证子模块、编译、大页内存和设备接管状态；不写盘、不实现 Cabe 自有 SPDK 工具。 |
| **P9M0-D14** | 文档同步范围 | 以 P9 最新设计为准，向前同步 P0 ~ P8 所有已完成文档中的后续路线描述。 |
| **P9M0-D15** | 退出条件 | 按文档、子模块、脚本、安全和环境验证五类核销；显式 NVMe 接管/释放列为验收项。 |
| **P9M0-D16** | 权限模型 | 高权限命令按需使用 `sudo`；root 用户下不加 `sudo`；执行前打印动作，失败即中断。 |
| **P9M0-D17** | SPDK 编译选项 | 使用 SPDK 默认 `./configure && make -j<N>`；不裁剪、不做 Cabe 链接优化。 |
| **P9M0-D18** | 文档对齐方式 | 由 Codex 完成全量语义同步；不要求用户人工判读，不做盲目批量替换。 |

---

## 3. 术语与边界

### 3.1 系统准备层

系统准备层指 Cabe 进程外完成的 SPDK 源码准备、依赖安装、SPDK 编译、大页内存配置和 NVMe 设备绑定。

P9M0 只处理系统准备层，不处理 Cabe 进程内 SPDK 运行时。

### 3.2 SPDK 运行环境

SPDK 运行环境指未来 Cabe 在 `Engine::Open` 中通过 `spdk_env_init` 初始化的进程内运行时。

该部分属于 P9M2 以后验证和 P9M4 以后封装的范围，P9M0 不调用 `spdk_env_init`。

### 3.3 设备接管

本文中的“设备接管”指把指定 NVMe controller 从 Linux 内核 `nvme` 驱动解绑，并绑定到 SPDK 可用的用户态 PCI 驱动，例如 `vfio-pci` 或 `uio_pci_generic`。

设备接管不等价于写盘。P9M0 允许接管和释放设备，但不执行任何读写校验。

### 3.4 设计权威

Cabe 的设计以已确认的最新设计为准。旧文档、旧路线图或旧上下文记忆与最新设计冲突时，以最新设计为准，并由当前里程碑负责同步文档。

P9M0 当前最新路线为：

| 阶段 | 定位 |
|---|---|
| P9 | SPDK NVMe API 后端接入 |
| P10 | 生产级无锁内存 B+树索引（学习与可选生产实现） |
| P11 | 多 NVMe 规模化与真盘验证 |
| P12 | 可观测性与运维工具 |

---

## 4. SPDK 子模块设计

### 4.1 子模块路径

SPDK 固定放在：

```text
third_party/spdk
```

P9M0 不使用：

```text
/home/pk/spdk
CABE_SPDK_ROOT
系统预装 SPDK
```

`/home/pk/spdk` 只属于前期人工验证痕迹，不进入 P9 设计、脚本、构建或文档依赖链。

### 4.2 版本锁定

P9M0 固定使用 SPDK `v26.01` 发布标签对应的提交。

实现时建议在脚本中固化：

```bash
SPDK_VERSION_TAG="v26.01"
SPDK_VERSION_FILE="26.01.0"
SPDK_LOCKED_COMMIT="2ef883ef96e79c3cc16da02f667a7a58c2453f2f"
```

说明：

- `refs/tags/v26.01` 是标签对象；
- `refs/tags/v26.01^{}` 对应实际提交；
- 脚本校验应使用实际提交 `2ef883ef96e79c3cc16da02f667a7a58c2453f2f`；
- 不使用可移动的 `LTS` 标签；
- 不跟踪 release 分支；
- 版本不匹配时直接失败，不自动切换。

### 4.3 初始化策略

`setup-spdk.sh init` 负责：

1. 检查 `third_party/spdk` 子模块路径；
2. 初始化递归子模块；
3. 校验当前 SPDK commit；
4. 校验 `VERSION` 文件；
5. 失败时直接退出并输出错误。

P9M0 脚本不得静默执行版本修复，例如自动 checkout、自动 reset 或自动切换分支。版本不一致表示当前工作区不符合 P9 基线，必须由开发者显式修复。

---

## 5. 脚本职责设计

### 5.1 `scripts/setup-dev.sh`

`setup-dev.sh` 继续保持 Cabe 基础开发环境入口：

```text
scripts/setup-dev.sh
  安装 Cabe 基础依赖
  保留 sync / io_uring / test / bench / coverage 依赖
  不初始化 SPDK 子模块
  不安装 SPDK 专用依赖
  不编译 SPDK
  不配置 hugepage
  不接管 NVMe 设备
```

普通开发、P8 以内回归、sync / `io_uring` 测试不应被 SPDK 重依赖污染。

### 5.2 `scripts/setup-spdk.sh`

`setup-spdk.sh` 是 SPDK 环境唯一入口。

支持子命令：

| 子命令 | 职责 | 是否高权限 |
|---|---|---|
| `init` | 初始化并校验 `third_party/spdk` 子模块 | 否 |
| `deps` | 调用 `third_party/spdk/scripts/pkgdep.sh` 安装依赖 | 是 |
| `build` | 执行 `./configure && make -j<N>` | 否 |
| `check` | 检查版本、commit、编译产物、大页内存和基础工具 | 否 |
| `hugepage` | 显式配置 SPDK 大页内存 | 是 |
| `all` | 执行 `init + deps + build + check` | 部分步骤需要 |
| `clean` | 清理 `third_party/spdk` 内的构建产物 | 否 |

`all` 固定定义为：

```text
all = init + deps + build + check
```

`all` 不包含：

- 大页内存配置；
- 设备接管；
- 设备释放；
- reset；
- SPDK 读写测试；
- Cabe 主工程构建。

### 5.3 `scripts/spdk-device.sh`

`spdk-device.sh` 是 NVMe 设备状态与接管入口。

支持子命令：

| 子命令 | 职责 | 是否写盘 |
|---|---|---|
| `status` | 只读展示 NVMe controller 状态和接管安全判断 | 否 |
| `bind` | 显式接管单个 BDF | 否，但会改变驱动绑定 |
| `unbind` | 显式释放单个 BDF | 否，但会改变驱动绑定 |

对外不暴露 `reset` 子命令。`unbind` 内部可以调用 SPDK 官方 `setup.sh reset`，但 Cabe 对外使用更准确的“释放设备”语义。

---

## 6. `setup-spdk.sh` 详细设计

### 6.1 命令形态

```bash
./scripts/setup-spdk.sh init
./scripts/setup-spdk.sh deps
./scripts/setup-spdk.sh build
./scripts/setup-spdk.sh build --jobs=8
./scripts/setup-spdk.sh check
./scripts/setup-spdk.sh hugepage --mem-mib=1024 --confirm
./scripts/setup-spdk.sh all
./scripts/setup-spdk.sh clean
```

### 6.2 fail-fast 策略

脚本严格执行 fail-fast：

| 场景 | 行为 |
|---|---|
| 子模块不存在 | `init` 可初始化；其他命令失败 |
| SPDK commit 不匹配 | 失败 |
| `VERSION` 不是 `26.01.0` | 失败 |
| 递归子模块缺失 | 失败 |
| 递归子模块偏离记录提交或发生冲突 | 失败 |
| `pkgdep.sh` 失败 | 失败 |
| `configure` 失败 | 失败 |
| `make` 失败 | 失败 |
| 编译产物缺失 | `check` 失败 |
| `/dev/hugepages` 不是 `hugetlbfs` 挂载 | `check` 失败 |
| 大页内存不足 | `check` 失败 |

脚本不允许：

- 静默跳过失败步骤；
- 自动修复子模块状态；
- 自动切换 SPDK 版本；
- 自动配置大页内存；
- 自动接管设备。

### 6.3 SPDK 编译

P9M0 使用 SPDK 默认编译策略：

```bash
cd third_party/spdk
./configure
make -j"$(nproc)"
```

`--jobs=N` 只影响 `make -j<N>`。

`check` 要求默认构建产生以下 P9M0 验证产物，任何一项缺失都直接失败：

```text
build/bin/spdk_tgt
build/bin/spdk_nvme_identify
build/bin/spdk_nvme_perf
build/examples/hello_world
build/lib/libspdk_nvme.a
```

P9M0 不讨论：

- 静态链接还是动态链接；
- Cabe CMake 如何查找 SPDK；
- 是否只编译 `libspdk_nvme`；
- 是否禁用示例或裁剪 bdev / nvmf / iscsi；
- LTO；
- debug / release 配置；
- pkg-config 输出。

这些留给 P9M1。

### 6.4 大页内存

P9M0 开发基线为：

```text
1024 MiB hugepage memory
```

显式配置命令：

```bash
./scripts/setup-spdk.sh hugepage --mem-mib=1024 --confirm
```

内部可以调用：

```bash
cd third_party/spdk
HUGEMEM=1024 ./scripts/setup.sh
```

`check` 必须只读检查：

- `/proc/meminfo` 可读；
- `/dev/hugepages` 已挂载；
- 2 MiB hugepage 总量满足 P9M0 基线；
- free hugepage 满足 P9M0 基线；
- 未接管设备不影响 `check` 的大页内存判断。

`all` 不自动配置大页内存。如果大页内存不足，`all` 在 `check` 阶段失败。

---

## 7. `spdk-device.sh` 详细设计

### 7.1 `status`

`status` 是只读诊断入口。

命令形态：

```bash
./scripts/spdk-device.sh status
./scripts/spdk-device.sh status --bdf=0000:13:00.0
```

默认展示全部 NVMe controller；带 `--bdf` 时只展示指定设备。

建议输出字段：

| 字段 | 含义 |
|---|---|
| BDF | PCIe 设备地址 |
| PCI type | 是否为 NVMe controller |
| vendor / device id | 设备标识 |
| driver | 当前内核驱动 |
| IOMMU group | IOMMU 分组 |
| SPDK status | 当前是否绑定到 SPDK 可用驱动 |
| block devices | 内核块设备名，例如 `nvme0n1` |
| size | 容量 |
| partitions | 分区列表 |
| mountpoints | 挂载点 |
| holders | LVM / dm / md 等 holder |
| system disk | 是否疑似系统盘 |
| eligibility driver | 本次安全判断采用的目标驱动，`status` 默认为 `vfio-pci` |
| bind eligibility | `safe` / `unsafe` / `already-bound` / `unknown` |
| reason | 安全判断原因 |

`status` 可以读取：

```text
lspci -nnk
lsblk
findmnt
/sys/bus/pci/devices/<BDF>
/sys/block/<dev>/holders
third_party/spdk/scripts/setup.sh status
```

SPDK 官方 `setup.sh status` 只作为补充视角，不能替代 Cabe 自己的系统盘、挂载点和 holder 判断。

### 7.2 `bind`

命令形态：

```bash
./scripts/spdk-device.sh bind --bdf=0000:13:00.0 --confirm-bind
./scripts/spdk-device.sh bind --bdf=0000:13:00.0 --driver=uio_pci_generic --confirm-bind
```

默认驱动：

```text
vfio-pci
```

允许显式驱动：

```text
vfio-pci
uio_pci_generic
```

不支持：

```text
bind-all
--all
多个 --bdf
--force
自动选择设备
自动驱动降级
igb_uio
```

`bind` 前置检查：

| 检查项 | 行为 |
|---|---|
| 未提供 `--bdf` | 失败 |
| 未提供 `--confirm-bind` | 失败 |
| BDF 不存在 | 失败 |
| BDF 不是 NVMe controller | 失败 |
| 指定驱动不在白名单 | 失败 |
| 当前已绑定到目标驱动 | 失败，提示 already bound |
| 当前绑定到另一个 SPDK 可用驱动 | 失败，要求先显式 `unbind` |
| 设备有挂载点 | 失败 |
| 设备有 holder | 失败 |
| 设备疑似系统盘 | 失败 |
| 目标驱动为 `vfio-pci` 且设备没有 IOMMU group | 失败；不允许隐式启用 VFIO no-IOMMU 模式 |
| 大页内存不足 | 失败 |
| SPDK 子模块未初始化或未编译 | 失败 |

底层调用形态：

```bash
cd third_party/spdk
SKIP_HUGE=yes PCI_ALLOWED="0000:13:00.0" DRIVER_OVERRIDE="vfio-pci" ./scripts/setup.sh
```

或：

```bash
cd third_party/spdk
SKIP_HUGE=yes PCI_ALLOWED="0000:13:00.0" DRIVER_OVERRIDE="uio_pci_generic" ./scripts/setup.sh
```

设备脚本必须传递 `SKIP_HUGE=yes`，确保 `bind` 不会再次执行大页分配。大页内存只能由显式的 `setup-spdk.sh hugepage --confirm` 修改。

底层命令返回后，脚本必须等待并确认目标 BDF 的当前驱动等于请求驱动；底层命令返回成功但驱动未切换时，`bind` 仍以失败结束。

### 7.3 `unbind`

命令形态：

```bash
./scripts/spdk-device.sh unbind --bdf=0000:13:00.0 --confirm-unbind
```

前置检查：

| 检查项 | 行为 |
|---|---|
| 未提供 `--bdf` | 失败 |
| 未提供 `--confirm-unbind` | 失败 |
| BDF 不存在 | 失败 |
| BDF 不是 NVMe controller | 失败 |
| 当前未绑定到 SPDK 可用驱动 | 失败 |
| SPDK 子模块不存在或 `setup.sh` 不可执行 | 失败 |

底层调用形态：

```bash
cd third_party/spdk
SKIP_HUGE=yes PCI_BLOCK_SYNC_ON_RESET=yes PCI_ALLOWED="0000:13:00.0" ./scripts/setup.sh reset
```

`reset` 只作为内部实现细节，不进入 Cabe 对外脚本命令。

底层命令返回后，脚本必须确认目标 BDF 已恢复到内核 `nvme` 驱动，并等待至少一个内核块设备映射重新出现；任一后置条件不满足时，`unbind` 直接失败并报告当前状态。

---

## 8. 权限模型

P9M0 采用最小权限提升模型。

规则：

```bash
SUDO=""
if [[ "${EUID}" -ne 0 ]]; then
    SUDO="sudo"
fi
```

只读命令默认不使用 root：

```text
setup-spdk.sh init
setup-spdk.sh build
setup-spdk.sh check
spdk-device.sh status
```

高权限命令按需使用 `sudo`：

```text
setup-spdk.sh deps
setup-spdk.sh hugepage
spdk-device.sh bind
spdk-device.sh unbind
```

如果脚本已经由 root 用户执行，则不额外加 `sudo`。

高权限动作执行前必须打印：

```text
action
target
key parameters
underlying command
```

示例：

```text
About to run privileged action:
  action: bind NVMe controller for SPDK
  bdf:    0000:13:00.0
  driver: vfio-pci
  command: env SKIP_HUGE=yes PCI_ALLOWED=0000:13:00.0 DRIVER_OVERRIDE=vfio-pci third_party/spdk/scripts/setup.sh
```

非 root 用户无 `sudo` 权限、底层命令失败或安全检查失败时，脚本直接错误退出。

---

## 9. 当前开发环境快照

P9M0 文档可以记录当前开发机快照，但该快照只作为人工操作参考，不进入脚本默认值和代码配置。

建议记录：

| 字段 | 说明 |
|---|---|
| 虚拟化环境 | VMware Workstation |
| SPDK 来源 | `third_party/spdk` 子模块 |
| SPDK 版本 | `v26.01` |
| 系统盘 BDF | 标记为禁止接管 |
| SPDK 候选 BDF | 用于 P9 显式接管 |
| 预期用途 | data / WAL / snapshot |
| 容量 | 例如 32G / 1G / 512M |
| 当前驱动 | 接管前通常是 `nvme`，接管后是 `vfio-pci` 或显式 `uio_pci_generic` |
| 备注 | VMware 虚拟 NVMe，只验证路径，不做性能结论 |

禁止在脚本中写入：

```bash
DEFAULT_DATA_BDF="..."
DEFAULT_WAL_BDF="..."
DEFAULT_SNAPSHOT_BDF="..."
```

设备用途的正式配置模型属于 P9M1：

```cpp
struct SpdkNvmeNamespaceConfig {
    std::string bdf;
    std::uint32_t nsid = 0;
};

struct SpdkDeviceConfig {
    SpdkNvmeNamespaceConfig data;
    SpdkNvmeNamespaceConfig wal;
    SpdkNvmeNamespaceConfig snapshot;
};
```

---

## 10. 文档同步设计

### 10.1 同步目标

P9M0 必须以 P9 最新设计为准，向前同步所有已完成文档中涉及后续路线的过期描述。

同步后的阶段定位：

| 阶段 | 最新定位 |
|---|---|
| P9 | SPDK NVMe API 后端接入 |
| P10 | 生产级无锁内存 B+树索引（学习与可选生产实现） |
| P11 | 多 NVMe 规模化与真盘验证 |
| P12 | 可观测性与运维工具 |

### 10.2 扫描范围

实现时必须扫描：

```text
README.md
ROADMAP.md
CONTEXT.md
doc/P0
doc/P1
doc/P2
doc/P3
doc/P4
doc/P4.5
doc/P5
doc/P6
doc/P7
doc/P8
doc/P9/README.md
bench
scripts
CMakeLists.txt
engine
io
index
wal
snapshot
test
```

建议扫描命令：

```bash
rg -n "P9|P10|P11|P12|SPDK|spdk|B\\+|BPlus|bplustree|CABE_SPDK_ROOT|/home/pk/spdk" README.md ROADMAP.md CONTEXT.md doc bench scripts CMakeLists.txt engine io index wal snapshot test
```

### 10.3 修改规则

| 旧描述 | 新描述 |
|---|---|
| P9 是 B+树 | P9 是 SPDK |
| P10 是 SPDK | P10 是 B+树 |
| SPDK 留给 P10 | SPDK 留给 P9 |
| B+树留给 P9 | B+树留给 P10 |
| `CABE_SPDK_ROOT` 是 P9 路径 | P9M0 不采用覆盖路径 |
| `/home/pk/spdk` 是 P9 路径 | 不进入 P9 设计 |
| P10 SPDK 大页内存 | P9 SPDK DMA / hugepage |

Codex 负责全量语义同步，不要求用户人工判读。同步时禁止无脑批量替换；已完成阶段真实发生过的历史实现事实保留，只修正其中过期的后续路线、阶段编号、依赖来源和债务流向。

### 10.4 实施后全量复查结果

P9M0 完成后的第二轮全量复查已覆盖根 README / ROADMAP / CONTEXT、`doc/P0` ~ `doc/P9` 和
`bench/baselines/README.md`。本轮在保留历史实现事实的前提下，进一步同步了：

- P9M0 已完成、下一步进入 P9M1 的当前状态；
- sync / `io_uring` / SPDK 的历史实现、过渡实现和长期方向；
- P9 `BDF + namespace id` 设备模型及仓库内 SPDK 子模块来源；
- `ValueBuffer`、应用端自备普通内存复制回退和 SPDK DMA 可用内存边界；
- WAL 专用设备抽象、snapshot 专用设备抽象和超级块轻量设备视图；
- P9 bench 只归档原始 JSON、不作性能结论的最新裁决；
- P11 真盘规模验证与 P12 可观测性/通用运维工具的边界；
- P9 SPDK 专属错误码段由 P9M1 落地的扩展纪律。

### 10.5 前向同步后的再次全量复查

在完成首轮前向同步后，再次逐文件复查根文档、P0～P8 全部 56 份阶段文档、P9 权威文档和
bench 归档说明。本轮补齐了首轮关键词扫描没有覆盖或无法识别的状态型遗漏：

- 将独立目录 `doc/P4.5` 纳入扫描，按 P7 实际实现修正“分配器内部原子化”和“P7 实现 TRIM”的早期预测；
- 统一 P0M7、P1M5、P2M2、P4M1～M4、P8M5 等收敛稿与详细稿的完成状态；
- 按 P7 最终结果修正 P1/P4/P5/P6 中关于 io_uring 真异步、定时刷出、后台快照、模糊快照、TRIM 和提交组收益的早期落点；
- 记录 P6M4 与 P8M5 已完成，补齐 P6～P8 bench 文件清单，并明确三阶段 loop 数据均不作性能结论；
- 修正 P7 的历史 Close 契约与 P8/P9 严格打开周期资源边界之间的演进关系；
- 修正 P2 冻结稿在 P5/P7/P8 后的返回类型、持久化、多设备和严格 Close 语义；
- 修正 P3 `MetaIndex` 的 8→6 方法计数、`IoBackend` 现行签名和历史未实现后端验证，补齐 P4 registered buffers 实际落点与 P8 最终 `IoWriteBuffer` 形态；
- 修正 ROADMAP 的 P7 最终里程碑、P8 完成状态和 D1～D26 附录摘要；
- 移除 P10 早期自管 snapshot 的旧计划，保留“生产级无锁内存 B+树”目标并把具体算法与里程碑裁决留给 P10；
- 为 P6 取消默认后端后的现行脚本接口补充说明，保留早期不带 `--backend` 命令作为历史实证。

本轮仍遵守“保留已发生历史、修正过期后续路线”的规则，不把 P9 尚未实施的 M1～M14 写成既成事实。

### 10.6 第三轮独立语义复查

在前两轮同步之后，又以当前代码常量、接口签名和 P9 最新设计为事实源，对 P0～P8 的 56 份文档
执行第三轮独立复查。本轮进一步修正了关键词扫描不容易发现的实现级偏差：

- 把已完成阶段首页中的“待梳理、策略待定、接下来编写”等当前时态改为历史问题或最终结果；
- 明确 `BlockId` 保存设备编号与逻辑块号，物理数据偏移还要由 I/O 后端加设备头部 8K；
- 将 ROADMAP 中路由、`N`、`R` 和 reactor 的单位统一为“设备组”，不再把一个 `DeviceContext`
  误写成单块 NVMe；P9 的物理角色进一步落到显式 namespace；
- 修正 ROADMAP 中 `ValueMeta` 的实际字段顺序，并注明 P5 通过 `WalFrame` / `SnapshotRecord`
  显式编码字段，不直接把整个 `ValueMeta` 对象写盘；
- 将 P4 的 `liburing` 接入从“策略待定”同步为系统 `liburing >= 2.9` + `pkg-config` 硬依赖；
- 在 P4 原始“第 0 块超级块”方案旁补齐 P5M1 最终的头部双份 4K 布局，避免把历史候选误认为现行格式；
- 补齐 P4 五方法 `IoBackend` 到 P8 六方法写入协议的演进，以及 registered buffers 已在 P8M4 兑现的事实；
- 修正 `wal_flush_interval_ms` 的生效状态：`wal_level` / `wal_buffer_size` 已生效，定时刷出字段仍存而不用；
- 把 ROADMAP 的 value 持久化要求改成后端中立的“持久化边界”语义，并记录 P5～P8 当前通过
  `fdatasync` 实现，而不是误写成已经使用设备 FUA 或“value 路径无 fsync”；
- 将 P8 总体文档从实施中时态收敛为已完成，并明确 `io_uring` 是 P8 过渡实现、SPDK 是 P9 长期方向。

本轮同样只修正文档事实和演进注记，不改写各里程碑当时真实发生的设计过程与验证记录。

---

## 11. 验证边界

### 11.1 P9M0 验证项

| 验证项 | 方式 | 是否写盘 |
|---|---|---|
| 子模块存在 | `git submodule status third_party/spdk` | 否 |
| SPDK 版本 | commit + `VERSION` 校验 | 否 |
| 依赖准备 | `setup-spdk.sh deps` 成功 | 否 |
| 编译完成 | `setup-spdk.sh build` 成功 | 否 |
| 编译产物 | `setup-spdk.sh check` | 否 |
| 大页内存 | `hugepage` 后 `check` | 否 |
| 设备状态 | `spdk-device.sh status` | 否 |
| 设备接管 | `spdk-device.sh bind --bdf=... --confirm-bind` | 不写盘，但改变驱动绑定 |
| 设备释放 | `spdk-device.sh unbind --bdf=... --confirm-unbind` | 不写盘，但改变驱动绑定 |

### 11.2 P9M0 不验证项

| 不验证项 | 后续里程碑 |
|---|---|
| Cabe 自有 `spdk_probe` 工具 | P9M2 |
| namespace 只读属性结构化输出 | P9M2 |
| qpair 创建 | P9M3 |
| completion 轮询 | P9M3 |
| SPDK DMA 内存分配 | P9M3 |
| 1 MiB 读写校验 | P9M3 |
| `SpdkNvmeDevice` | P9M4 |
| `SpdkIoBackend` | P9M6 |
| Engine SPDK 端到端 | P9M12 |

### 11.3 手动验证流程

P9M0 完成后，建议手动执行：

```bash
./scripts/setup-spdk.sh init
./scripts/setup-spdk.sh deps
./scripts/setup-spdk.sh hugepage --mem-mib=1024 --confirm
./scripts/setup-spdk.sh build
./scripts/setup-spdk.sh check
./scripts/spdk-device.sh status
./scripts/spdk-device.sh bind --bdf=<目标BDF> --confirm-bind
./scripts/spdk-device.sh status --bdf=<目标BDF>
./scripts/spdk-device.sh unbind --bdf=<目标BDF> --confirm-unbind
./scripts/spdk-device.sh status --bdf=<目标BDF>
```

同时需要确认现有路径不被污染：

```bash
./scripts/run-tests.sh --backend=sync --release --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2
./scripts/run-tests.sh --backend=io_uring --release --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2
```

---

## 12. 实施顺序

建议实施顺序：

1. 新增本文档。
2. 引入 `third_party/spdk` 子模块并固定 `v26.01` 对应提交。
3. 编写 `scripts/setup-spdk.sh`。
4. 编写 `scripts/spdk-device.sh status`。
5. 编写 `scripts/spdk-device.sh bind/unbind`。
6. 验证 `setup-spdk.sh init/deps/hugepage/build/check`。
7. 验证 `spdk-device.sh status/bind/unbind`。
8. 运行 sync / `io_uring` 现有回归。
9. 使用 `rg` 扫描并同步 README、ROADMAP、CONTEXT、doc/P0 ~ doc/P9、bench、scripts、代码注释中的路线引用。
10. 更新 P9M0 退出条件核销状态。

---

## 13. 退出条件

P9M0 完成时必须满足：

1. 本文档存在并记录 P9M0-D1 ~ P9M0-D18。
2. P9/P10/P11/P12 最新路线已同步到 README、ROADMAP、CONTEXT、doc/P0 ~ doc/P9 和相关脚本 / 注释。
3. `third_party/spdk` 子模块存在。
4. SPDK 子模块固定到 `v26.01` 对应提交 `2ef883ef96e79c3cc16da02f667a7a58c2453f2f`。
5. `third_party/spdk/VERSION` 为 `26.01.0`。
6. `scripts/setup-spdk.sh init` 可初始化并校验子模块。
7. `scripts/setup-spdk.sh deps` 可调用 SPDK 官方依赖脚本。
8. `scripts/setup-spdk.sh hugepage --mem-mib=1024 --confirm` 可显式配置 1 GiB 大页内存。
9. `scripts/setup-spdk.sh build` 可完成 `./configure && make -j<N>`。
10. `scripts/setup-spdk.sh check` 可校验版本、commit、编译产物、大页内存和基础工具。
11. `scripts/setup-spdk.sh all` 只执行 `init + deps + build + check`，不配置大页内存，不接管设备。
12. `scripts/spdk-device.sh status` 可只读展示 NVMe controller 状态和安全判断。
13. `scripts/spdk-device.sh bind --bdf=<BDF> --confirm-bind` 可显式接管指定 NVMe。
14. `scripts/spdk-device.sh unbind --bdf=<BDF> --confirm-unbind` 可显式释放指定 NVMe。
15. 设备接管不支持批量、自动选择、多 BDF、`--force` 或驱动自动降级。
16. 默认驱动为 `vfio-pci`；显式允许 `uio_pci_generic`。
17. 高权限命令按需使用 `sudo`；root 用户下不额外使用 `sudo`。
18. P9M0 不实现 Cabe 自有 SPDK probe、不创建 qpair、不分配 SPDK DMA、不做读写验证。
19. sync 和 `io_uring` 现有构建 / 测试不因 P9M0 变化失效。

### 13.1 本次实现核销

已完成：

- `third_party/spdk` 子模块接入并锁定到 `v26.01` 对应提交 `2ef883ef96e79c3cc16da02f667a7a58c2453f2f`。
- `scripts/setup-spdk.sh` 已实现 `init/deps/build/check/hugepage/all/clean`，其中 `hugepage` 必须显式 `--confirm`。
- `scripts/spdk-device.sh` 已实现 `status/bind/unbind`，设备接管只允许单 BDF、强确认、无批量、无自动选择、无 `--force`。
- README、ROADMAP、CONTEXT、P0~P9 全量相关文档和 bench 归档说明已经同步到 P9 最新路线；不仅修正 P9/P10 阶段映射，也同步了设备模型、内存、WAL/snapshot/超级块抽象、bench 和 P11/P12 债务边界。
- 已验证脚本语法、帮助输出、子模块版本、只读设备状态和现有 sync Release 回归。

保留为手动执行：

- `setup-spdk.sh deps/build/hugepage/check/all` 中会安装依赖、编译 SPDK 或改变大页内存状态的部分。
- `spdk-device.sh bind/unbind` 会改变 NVMe controller 驱动绑定状态，必须由开发者显式选择目标 BDF 后执行。

---

## 14. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| SPDK 版本漂移 | 后续编译、API、脚本行为不可复现 | 固定 `v26.01` 对应提交并在脚本中校验 |
| 误接管系统盘 | 可能导致系统不可用 | `status` 和 `bind` 检查挂载点、holder、系统盘；无 `--force` |
| 批量绑定误操作 | 多块设备被错误接管 | 禁止 `bind-all`、多 BDF 和自动选择 |
| 驱动自动降级 | 安全语义变化不透明 | 默认 `vfio-pci`，`uio_pci_generic` 必须显式指定 |
| 缺少 IOMMU group 时使用 VFIO | 绑定失败或落入不安全的 no-IOMMU 语义 | `status` 明确显示默认驱动可绑定性；`bind` 在调用底层脚本前失败 |
| holder 漏报 | 未挂载但仍被 LVM、dm 或 md 使用的设备可能被误接管 | 跟随 `/sys/block/<dev>` 符号链接并遍历磁盘及其分区的 holder |
| `setup-dev.sh` 被 SPDK 污染 | 普通开发环境变重变危险 | SPDK 独立到 `setup-spdk.sh` |
| 大页内存隐式改变 | 系统内存状态被悄悄修改 | `hugepage` 必须显式 `--confirm`；`all` 不自动配置；设备 `bind/unbind` 固定传递 `SKIP_HUGE=yes` |
| 文档旧路线残留 | 后续设计和实现被旧 P9/P10 描述误导 | P9M0 全量扫描并语义同步 |
| 过早进入 SPDK 主路径 | M0 范围膨胀 | P9M0 不接 CMake、不链接 SPDK、不实现 probe/qpair/DMA/读写 |

---

## 15. 后续入口

P9M0 完成后，P9M1 开始处理 Cabe 主工程与 SPDK 的正式构建关系：

- `CABE_IO_BACKEND=spdk` 构建接入；
- SPDK 头文件和库查找；
- `Options::spdk_devices` 类型化配置；
- SPDK 错误码段；
- SPDK 测试环境变量和默认跳过策略。

P9M2 开始实现 Cabe 自有只读探测工具；P9M3 再进入 qpair、DMA 和 1 MiB 读写验证。
