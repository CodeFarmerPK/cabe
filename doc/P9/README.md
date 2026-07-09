# P9 - SPDK NVMe API 后端接入 · 总体里程碑设计

状态：🚧 设计完成，待各里程碑详细设计与实现

## 1. 阶段目标

P9 的目标是把 Cabe 的 I/O 主路径从当前 `io_uring` 过渡后端推进到 SPDK 后端，并直接基于 SPDK NVMe API 接入 NVMe 设备。

P9 不走 SPDK bdev 路线，也不把 SPDK 作为外部服务使用。Cabe 进程直接链接 SPDK 库，在 `Engine::Open` 中初始化进程内 SPDK 运行时，按显式配置探测 controller 和 namespace，并通过 SPDK NVMe API 完成 value/data、WAL、snapshot 和超级块读写。

P9 采用小步里程碑推进：

- 先在 Cabe 外部工具中验证 SPDK 最小能力；
- 再引入内部 `SpdkNvmeDevice` 薄封装；
- 再按 value/data、WAL、snapshot 的顺序逐步接入 Cabe；
- 最后完成全 SPDK 模式的端到端验证、测试、bench 和文档收敛。

P9 的主要目标是功能正确、语义清晰和学习路径可控。当前 VMware 虚拟 NVMe 环境下的性能数据只作为路径打通和历史样本，不作为真实 SPDK 性能结论。

## 2. 总体定位

P9 是 P8 零拷贝主路径之后的直接延续。P8 已经引入：

- `ValueBuffer`；
- `ValueBufferPool`；
- `IoWriteBuffer`；
- 后端中立的写入缓冲区描述符；
- `io_uring` 注册缓冲区验证路径。

P9 将这些抽象落到 SPDK 的 DMA 可用内存和 NVMe 命令提交路径上。

路线图定位同步调整为：

| 阶段 | 调整后定位 |
| --- | --- |
| P9 | SPDK NVMe API 后端接入 |
| P10 | B+树索引学习路径 |
| P11 | 多 NVMe 规模化与真盘验证 |
| P12 | 可观测性与运维工具 |

P9 需要保持以下长期方向：

- `io_uring` 继续作为过渡实现和回归后端；
- SPDK 是后续高性能 I/O 主方向；
- 公开 `Put` 接口不拆分；
- SPDK 细节不暴露给上层应用；
- 应用端通过 Cabe 的 `AllocateValueBuffer(key)` 获得 SPDK 零拷贝能力；
- 应用端自备普通内存在 SPDK 后端下复制回退。

## 3. 术语边界

| 术语 | 含义 |
| --- | --- |
| SPDK 运行环境 | Cabe 进程内通过 `spdk_env_init` 初始化的 SPDK/DPDK 运行时环境。它不同于系统层设备绑定。 |
| 系统准备层 | Cabe 进程外完成的 SPDK 源码准备、编译、hugepage 配置和 NVMe 设备绑定。 |
| BDF | PCIe 设备地址，例如 `0000:13:00.0`，用于标识 NVMe controller。 |
| namespace id | NVMe controller 内部的 namespace 编号，例如 `1`、`2`、`3`。 |
| SPDK 设备组 | 一组显式配置的 data、WAL、snapshot namespace，三者共同组成 Cabe 的一个逻辑设备组。 |
| `SpdkNvmeDevice` | Cabe 内部对 SPDK NVMe controller、namespace、qpair、LBA 换算、DMA 内存和 completion 轮询的薄封装。 |
| qpair | SPDK NVMe I/O queue pair。P9 中 qpair 是 reactor 私有资源。 |
| SPDK DMA 内存 | 由 SPDK DMA 分配接口或 Cabe 封装的 SPDK DMA 内存池分配出的设备可访问内存。 |
| 可工作路径 | 先保证功能正确，可以复制到 SPDK DMA 缓冲区后提交 I/O，不要求零拷贝。 |
| SPDK 零拷贝路径 | Cabe `ValueBuffer` 底层来自 SPDK DMA 内存，`Put` 命中 P8 主路径后直接提交给 SPDK write。 |

## 4. P9 范围

P9 包含：

- 正式把 P9 阶段定位为 SPDK 接入，并将 B+树后移；
- 将 SPDK 作为 `third_party/spdk` 子模块纳入 Cabe 的依赖管理；
- 新增 SPDK 环境准备、编译、检查和设备绑定辅助脚本；
- 新增类型化 SPDK 设备配置；
- 新增 SPDK 专属错误码段；
- 新增 SPDK 独立验证工具；
- 新增 `SpdkRuntime`、`SpdkNvmeDevice` 和 DMA 内存封装；
- 新增 `SpdkIoBackend`，让 value/data 路径走 SPDK；
- 将 P8 `ValueBufferPool` 接入 SPDK DMA 内存；
- 将 WAL 从 `RawDevice` 迁移到 WAL 专用设备抽象，并新增 SPDK 实现；
- 将 snapshot 从 `RawDevice` 迁移到 snapshot 专用设备抽象，并新增 SPDK 实现；
- 将超级块读写从 `RawDevice` 迁移到超级块轻量设备视图，并新增 SPDK 适配；
- 完成全 SPDK 模式 create、recover、Put、Get、Delete、Snapshot、Close 的端到端验证；
- 补齐 SPDK 测试、bench、归档和文档。

P9 不包含：

- SPDK bdev 路线；
- 外部 SPDK 服务或跨进程 SPDK 访问协议；
- 公开 SPDK 初始化 / 关闭 API；
- 公开 SPDK DMA 内存导入接口；
- 应用端直接管理 SPDK DMA 内存；
- 多请求在飞；
- poll group；
- qpair 队列深度调优；
- WAL/data 并行化；
- `reactor` 内多 qpair 性能调优；
- 真实硬件性能结论；
- B+树索引实现。

## 5. 已锁定设计决策

| 决策点 | 结论 |
| --- | --- |
| P9-D1 阶段定位 | P9 正式定位为 SPDK NVMe API 后端接入；B+树索引学习路径后移到 P10。 |
| P9-D2 接入路线 | 直接基于 SPDK NVMe API 接入，不先走 SPDK bdev。 |
| P9-D3 里程碑切分 | 按“可运行证据”小步推进，先 Cabe 外部验证 SPDK，再逐步接入 value/data、WAL、snapshot。 |
| P9-D4 设备配置模型 | 新增类型化 SPDK 配置，显式使用 `BDF + namespace id`，不使用伪路径字符串作为长期模型。 |
| P9-D5 namespace 用途映射 | 每个设备组显式配置 data、WAL、snapshot namespace；代码不硬编码 `nsid 1/2/3` 用途，文档中将当前测试环境约定为 `1=data, 2=WAL, 3=snapshot`。 |
| P9-D6 构建与环境 | SPDK 后续作为 `third_party/spdk` 子模块纳入 Cabe；`CABE_SPDK_ROOT` 仅作为本地覆盖路径；SPDK 环境准备、编译、检查和设备绑定由 Cabe 脚本管理，设备绑定必须显式执行。 |
| P9-D7 生命周期 | SPDK 后端在 `Engine::Open` 中初始化 Cabe 进程内 SPDK 运行时，在 `Engine::Close` 中释放本次打开周期所有 SPDK 资源并调用 `spdk_env_fini`；重复 Open 只支持同参数，不一致则失败。 |
| P9-D8 探测策略 | 只按 `Options::spdk_devices` 中显式出现的 BDF 定向 probe / attach；同一 BDF 只 attach 一次；namespace 按配置 `nsid` 获取，不自动选择设备。 |
| P9-D9 独立验证工具 | 在改 Cabe 主路径前先提供 Cabe 自带 SPDK 验证工具，覆盖只读探测、namespace 枚举、DMA 分配、qpair 创建和 1MiB 读写校验。 |
| P9-D10 内部封装 | 先引入内部 `SpdkNvmeDevice` 薄封装，再在其上适配 `SpdkIoBackend`、WAL、snapshot 和超级块。 |
| P9-D11 qpair 所有权 | qpair 是 reactor 私有资源；controller attach 和 namespace 解析可在 Open 阶段完成，qpair 创建和释放发生在所属 reactor 线程内。 |
| P9-D12 completion 轮询 | P9 初期采用同步式 completion 轮询，不引入多请求在飞、独立 poller 线程、poll group 或事件通知。 |
| P9-D13 value/data 接入 | value/data 分两步：先实现可工作路径，允许复制；再接入 P8 `ValueBufferPool` 和 SPDK DMA 内存形成零拷贝路径。 |
| P9-D14 DMA 内存策略 | SPDK 后端下提交给 NVMe 的所有 I/O payload 必须来自 Cabe 可识别的 SPDK DMA 内存。 |
| P9-D15 应用端内存策略 | SPDK 下应用端自备普通内存复制回退；`AllocateValueBuffer(key)` 内部使用 Cabe 自管 SPDK DMA 内存，对应用端透明，并作为 SPDK 零拷贝主路径。 |
| P9-D16 WAL 设备抽象 | WAL 从直接依赖 `RawDevice` 迁移到 WAL 专用设备抽象；Raw 和 SPDK 各自适配；不改变 WAL 格式、环形日志算法和恢复算法。 |
| P9-D17 WAL 持久化语义 | 支持 Flush 时发 NVMe Flush；Flush 不支持但无易失写缓存时以 write completion 作为同步边界；否则 WAL 同步语义不成立，Open 或切档失败。 |
| P9-D18 snapshot 设备抽象 | snapshot 从 `RawDevice` 迁移到 snapshot 专用设备抽象；Raw 和 SPDK 各自适配；不改变 A/B 槽格式和恢复逻辑。 |
| P9-D19 超级块读写 | 超级块读写迁移到超级块轻量设备视图；Raw 和 SPDK 共用一份超级块格式和身份校验逻辑；Open 阶段可使用临时 qpair。 |
| P9-D20 错误码与诊断 | 新增 SPDK 专属错误码段；公开 `Status` 仍保持简单错误码模型；日志记录 BDF、namespace id、用途、操作、offset/length/LBA、SPDK 返回码和 completion 状态。 |
| P9-D21 测试策略 | SPDK 测试纳入测试体系但默认安全跳过；显式配置 BDF/nsid 后运行只读测试；显式写盘确认后才运行写入和 Engine SPDK 端到端测试。 |
| P9-D22 bench 与归档 | P9 采集并归档 SPDK bench 原始 JSON，但不做性能结论、不设置性能门槛；VMware 虚拟 NVMe 数据只作为路径样本。 |

## 6. 里程碑拆分

P9 拆分为 15 个实施里程碑：

```mermaid
flowchart LR
    M0["M0 阶段重排与 SPDK 环境基线"]
    M1["M1 构建接入与配置模型"]
    M2["M2 定向探测与只读验证工具"]
    M3["M3 qpair、DMA 与读写验证"]
    M4["M4 SpdkNvmeDevice 薄封装"]
    M5["M5 超级块设备视图"]
    M6["M6 value/data 可工作路径"]
    M7["M7 value/data 零拷贝路径"]
    M8["M8 WAL 设备抽象"]
    M9["M9 WAL SPDK 接入"]
    M10["M10 snapshot 设备抽象"]
    M11["M11 snapshot SPDK 接入"]
    M12["M12 全 SPDK 端到端"]
    M13["M13 多设备与 qpair 收敛"]
    M14["M14 测试、bench 与文档收敛"]

    M0 --> M1 --> M2 --> M3 --> M4 --> M5 --> M6 --> M7 --> M8 --> M9 --> M10 --> M11 --> M12 --> M13 --> M14
```

| 里程碑 | 文档 | 状态 | 核心目标 |
| --- | --- | --- | --- |
| P9M0 | `P9M0_spdk_environment_design.md` | ⏳ 待详细设计 | 完成阶段重排、SPDK 子模块规划、环境脚本和设备绑定安全边界。 |
| P9M1 | `P9M1_build_config_design.md` | ⏳ 待详细设计 | 接入 SPDK 构建、类型化配置、错误码段和测试环境变量框架。 |
| P9M2 | `P9M2_probe_tool_design.md` | ⏳ 待详细设计 | 实现 SPDK 定向 probe、namespace 只读枚举和安全验证工具。 |
| P9M3 | `P9M3_qpair_dma_rw_verify_design.md` | ⏳ 待详细设计 | 跑通 qpair、DMA 内存、completion 轮询和 1MiB 读写校验。 |
| P9M4 | `P9M4_spdk_nvme_device_design.md` | ⏳ 待详细设计 | 引入内部 `SpdkNvmeDevice` 薄封装和严格生命周期管理。 |
| P9M5 | `P9M5_superblock_device_view_design.md` | ⏳ 待详细设计 | 将超级块读写迁移到轻量设备视图，补 SPDK 适配和 Open 阶段临时 qpair。 |
| P9M6 | `P9M6_value_spdk_working_path_design.md` | ⏳ 待详细设计 | 实现 `SpdkIoBackend` value/data 可工作路径，允许复制，验证 `Put/Get/Delete`。 |
| P9M7 | `P9M7_value_spdk_zero_copy_design.md` | ⏳ 待详细设计 | 将 `ValueBufferPool` 接入 SPDK DMA 内存，完成 value 零拷贝主路径。 |
| P9M8 | `P9M8_wal_device_abstraction_design.md` | ⏳ 待详细设计 | 将 WAL 从 `RawDevice` 解耦到 WAL 专用设备抽象，保持 Raw 路径行为不变。 |
| P9M9 | `P9M9_wal_spdk_design.md` | ⏳ 待详细设计 | WAL 接入 SPDK，落实 DMA 缓冲区和设备能力驱动的同步语义。 |
| P9M10 | `P9M10_snapshot_device_abstraction_design.md` | ⏳ 待详细设计 | 将 snapshot 从 `RawDevice` 解耦到 snapshot 专用设备抽象，保持 Raw 路径行为不变。 |
| P9M11 | `P9M11_snapshot_spdk_design.md` | ⏳ 待详细设计 | snapshot 接入 SPDK，完成 A/B 槽读写、校验、加载和同步语义验证。 |
| P9M12 | `P9M12_full_spdk_e2e_design.md` | ⏳ 待详细设计 | 完成 value、WAL、snapshot、superblock 全 SPDK 模式端到端 create/recover。 |
| P9M13 | `P9M13_multidevice_qpair_convergence_design.md` | ⏳ 待详细设计 | 多设备 SPDK 验证，收敛 qpair 所有权、Close/Open 严格语义和设备隔离。 |
| P9M14 | `P9M14_test_bench_convergence_design.md` | ⏳ 待详细设计 | 完成测试矩阵、bench 数据归档、文档收敛和 P9 阶段状态同步。 |

## 7. P9M0 - 阶段重排与 SPDK 环境基线

目标：

- 将 P9 正式定位为 SPDK，B+树后移；
- 将 SPDK 作为 `third_party/spdk` 子模块纳入长期规划；
- 固化当前开发环境与正式环境之间的关系；
- 设计 SPDK 环境准备、编译、hugepage 检查和设备绑定脚本；
- 明确设备绑定是显式高风险操作，不由 `setup-dev.sh` 默认执行。

建议脚本边界：

| 脚本 | 职责 |
| --- | --- |
| `scripts/setup-dev.sh` | 安装 Cabe 基础依赖，可安装 SPDK 所需系统依赖，但不默认绑定设备。 |
| `scripts/setup-spdk.sh` | 初始化 / 更新 SPDK 子模块，安装 SPDK 依赖，编译 SPDK，检查 hugepage。 |
| `scripts/spdk-device.sh` | 显式执行 `status`、`bind`、`unbind`、`reset`，只操作传入或白名单 BDF。 |

P9M0 退出条件：

- P9/P10/P11/P12 阶段重排方案同步到后续文档计划；
- SPDK 子模块路径和本地覆盖路径语义定稿；
- 设备绑定安全边界定稿；
- 当前 `/home/pk/spdk` 被标注为临时验证路径。

## 8. P9M1 - 构建接入与配置模型

目标：

- 为 `CABE_IO_BACKEND=spdk` 接入 SPDK 构建；
- 默认从 `third_party/spdk` 查找 SPDK，允许 `CABE_SPDK_ROOT` 本地覆盖；
- 新增类型化 SPDK 设备配置；
- 新增 SPDK 错误码段；
- 新增 SPDK 测试环境变量解析和默认跳过策略。

建议配置形态：

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

struct Options {
    std::vector<DeviceConfig> devices;
    std::vector<SpdkDeviceConfig> spdk_devices;
};
```

P9M1 退出条件：

- `sync` / `io_uring` 构建不依赖 SPDK；
- `spdk` 构建能找到 SPDK 头文件和库；
- 缺少 SPDK 依赖时 CMake 报错清晰；
- SPDK 配置能表达多设备组；
- 重复 namespace 能在 Open 前被拒绝；
- SPDK 错误码段已落地。

## 9. P9M2 - 定向探测与只读验证工具

目标：

- 新增 Cabe 自带 SPDK 只读验证工具；
- 实现 `spdk_env_init` / `spdk_env_fini` 的严格生命周期原型；
- 按显式 BDF 定向 probe / attach；
- 枚举 namespace 并输出只读属性；
- 验证当前 VMware 虚拟 NVMe 的 namespace 布局。

只读验证工具需要覆盖：

- BDF 是否存在；
- namespace id 是否存在；
- namespace 容量；
- LBA 大小；
- 最大传输大小；
- Flush 支持；
- 易失写缓存声明；
- controller attach / detach 回滚。

P9M2 退出条件：

- 不写盘的 `spdk_probe` 类工具可运行；
- 未显式配置 BDF 时不自动选择设备；
- 未配置设备时测试安全跳过；
- attach 失败能回滚已 attach controller；
- 文档记录当前测试设备的只读探测结果。

## 10. P9M3 - qpair、DMA 与读写验证

目标：

- 验证 SPDK qpair 创建和释放；
- 验证 SPDK DMA 内存分配和释放；
- 验证同步式 completion 轮询；
- 对指定 namespace 执行 1MiB 写入、读取和 CRC 校验；
- 建立写盘保护机制。

写入验证必须显式开启，例如：

```bash
export CABE_SPDK_ALLOW_WRITE_TESTS=1
```

P9M3 不接入 Engine 主路径。它只证明当前机器、当前 SPDK、当前设备可以被 Cabe 代码通过 SPDK NVMe API 正确读写。

P9M3 退出条件：

- DMA buffer 分配成功，地址满足约定对齐；
- qpair 在同一线程内创建、使用、释放；
- `spdk_nvme_ns_cmd_write` / `spdk_nvme_ns_cmd_read` 可完成 1MiB 校验；
- completion 超时和 completion 错误能转换为 SPDK 错误码；
- 默认不写盘，缺少危险确认时写入测试跳过。

## 11. P9M4 - `SpdkNvmeDevice` 薄封装

目标：

- 将 M2/M3 的原型能力收敛到内部 `SpdkNvmeDevice`；
- 封装 controller、namespace、qpair、LBA 换算、DMA 约束、completion 轮询和错误转换；
- 提供按字节偏移的 `ReadAt` / `WriteAt` / `Flush` / `SizeBytes` 能力；
- 落实 P9-D7 的严格 Open/Close 生命周期。

`SpdkNvmeDevice` 不进入公开 API，不处理 key、WAL entry、snapshot record 等上层语义。

P9M4 退出条件：

- `SpdkNvmeDevice` 可被工具复用；
- offset / len 到 LBA 的换算有单元测试；
- 超过最大传输能力时有明确错误或明确拆分策略；
- Close 后释放 qpair、controller 和 DMA 内存；
- 同一进程内重复 Open 只支持同参数，不一致时失败。

## 12. P9M5 - 超级块设备视图

目标：

- 将超级块读写从直接依赖 `RawDevice` 迁移到轻量设备视图；
- Raw 和 SPDK 共用同一份超级块格式和身份校验逻辑；
- 在 SPDK 模式下支持三类 namespace 的超级块 create / recover；
- 在 Open 阶段使用临时 qpair 完成超级块读写。

超级块逻辑必须保持：

- 双份超级块；
- 主备修复；
- `engine_uuid`；
- `device_uuid`；
- 配对 UUID；
- `device_type`；
- `device_id`；
- `device_count`；
- `block_count` 校验。

P9M5 退出条件：

- Raw 路径超级块测试保持通过；
- SPDK 路径可以写入并恢复三类设备超级块；
- 设备类型不匹配、配对不匹配、设备数量不匹配时拒绝打开；
- Open 阶段临时 qpair 使用后释放；
- 运行期 qpair 所有权仍归 reactor。

## 13. P9M6 - value/data SPDK 可工作路径

目标：

- 新增 `SpdkIoBackend`；
- 让 value/data 的 `Write(block_idx, IoWriteBuffer)` 和 `Read(block_idx, rbuf)` 走 SPDK；
- 先允许内部复制到 SPDK DMA fallback buffer；
- 验证 `Put` / `Get` / `Delete` 基本行为；
- 验证 value CRC、block 回收和恢复期 value 读取。

P9M6 的重点是可工作路径，不要求 `ValueBuffer` 立即零拷贝。

P9M6 退出条件：

- `CABE_IO_BACKEND=spdk` 能构建；
- value/data namespace 的 `block_idx` 寻址正确；
- `kDataRegionOffset` 仍然生效；
- `Put` 后 `Get` 能读回 1MiB value；
- `Delete` 和覆盖写语义不退化；
- 普通外部 value 内存复制到 SPDK DMA buffer 后写入。

## 14. P9M7 - value/data SPDK 零拷贝路径

目标：

- 将 P8 `ValueBufferPool` 的 SPDK 后端内存来源切换为 SPDK DMA 内存；
- 让 `AllocateValueBuffer(key)` 在 SPDK 下返回 DMA 可用 `ValueBuffer`；
- 让 `Put(key, buffer.view())` 在 key、设备和 slot 匹配时走 SPDK 零拷贝；
- 让应用端自备普通内存继续复制回退；
- 让复制回退缓冲区也来自 SPDK DMA 内存。

SPDK 下三类 value 来源规则：

| value 来源 | 处理方式 |
| --- | --- |
| Cabe `ValueBuffer` 且绑定 key 匹配 | SPDK 零拷贝主路径。 |
| Cabe `ValueBuffer` 但 key 或设备不匹配 | 复制回退。 |
| 应用端自备普通内存 | 复制回退。 |

P9M7 退出条件：

- `ValueBuffer` 底层来自 SPDK DMA 内存；
- `ValueBuffer` 主路径不再复制 1MiB value；
- key 不匹配和跨设备场景仍复制回退；
- Close 等待 `ValueBuffer` 释放后再释放 SPDK DMA 资源；
- P8 原有 value buffer 测试在 SPDK 后端下有对应覆盖。

## 15. P9M8 - WAL 设备抽象

目标：

- 将 WAL 从直接持有 `RawDevice` 迁移到 WAL 专用设备抽象；
- 保留 Raw 适配实现；
- 不改变 WAL 帧格式、环形日志算法、恢复扫描和回收语义；
- 为 SPDK WAL 接入准备按偏移读写接口。

WAL 设备抽象需要表达：

- `SizeBytes`；
- `ReadAt`；
- `WriteAt`；
- `Sync`；
- `Close`。

P9M8 退出条件：

- Raw WAL 路径行为不变；
- WAL 单元测试和恢复测试保持通过；
- WAL 主逻辑不包含 SPDK 头文件；
- WAL 设备抽象可被 SPDK 适配实现替换。

## 16. P9M9 - WAL SPDK 接入

目标：

- 新增基于 `SpdkNvmeDevice` 的 WAL 设备实现；
- 将 WAL 写缓冲区、恢复扫描缓冲区切换为 SPDK DMA 内存；
- 落实 WAL 同步能力判断；
- 验证 WAL write、Flush、recover 和 ring reclaim。

WAL 同步能力规则：

| 设备能力 | `Sync()` 语义 |
| --- | --- |
| 支持 Flush | 提交 NVMe Flush 并等待完成。 |
| Flush 不支持但无易失写缓存 | 以已完成 write completion 作为同步边界。 |
| Flush 不支持且存在或无法确认易失写缓存 | 同步语义不成立，Open 或切档失败。 |

P9M9 退出条件：

- SPDK WAL 能写入和恢复；
- 同步 WAL 级别按设备能力判断；
- 不静默降级 WAL 级别；
- `SetWalLevel` 切到同步档时会检查能力；
- 当前 VMware 虚拟 NVMe 能按“无易失写缓存”能力路径通过功能验证。

## 17. P9M10 - snapshot 设备抽象

目标：

- 将 snapshot 从直接持有 `RawDevice` 迁移到 snapshot 专用设备抽象；
- 保留 Raw 适配实现；
- 不改变 A/B 双槽格式、槽头校验、generation 选择、坏槽回退和 data CRC 逻辑；
- 为 SPDK snapshot 接入准备按偏移读写接口。

P9M10 退出条件：

- Raw snapshot 路径行为不变；
- snapshot 单元测试和恢复测试保持通过；
- snapshot 主逻辑不包含 SPDK 头文件；
- snapshot 设备抽象可被 SPDK 适配实现替换。

## 18. P9M11 - snapshot SPDK 接入

目标：

- 新增基于 `SpdkNvmeDevice` 的 snapshot 设备实现；
- 将槽头缓冲区、流式写缓冲区、校验读缓冲区和 Load 缓冲区切换为 SPDK DMA 内存；
- 复用 D17 的设备能力驱动 `Sync()` 语义；
- 验证 A/B 双槽写入、读取、校验、坏槽回退和加载。

P9M11 退出条件：

- SPDK snapshot 能手动写入和恢复加载；
- 自动快照失败时不回收 WAL；
- snapshot Sync 失败能正确返回或记录错误；
- A/B 槽语义与 Raw 路径一致；
- SPDK snapshot buffer 全部来自 DMA 内存。

## 19. P9M12 - 全 SPDK 端到端

目标：

- value/data、WAL、snapshot、superblock 全部走 SPDK；
- 完成 `create=true` 初始化；
- 完成 recover 打开；
- 覆盖 `Put`、`Get`、`Delete`、`Snapshot`、`SetWalLevel`、`Close`；
- 清理过渡混合路径。

P9M12 退出条件：

- 单设备组全 SPDK create 成功；
- 单设备组 recover 成功；
- `Put/Get/Delete` 语义与 `io_uring` 路径一致；
- 手动 snapshot 后 WAL 能按边界回收；
- Close 后旧打开周期资源全部失效；
- 不需要内核块设备路径即可完成完整 Engine 生命周期。

## 20. P9M13 - 多设备与 qpair 收敛

目标：

- 支持两个 SPDK 设备组；
- 验证 `hash(key) % N` 路由在 SPDK 下仍正确；
- 验证每个 reactor 的 qpair 私有所有权；
- 验证 Open 阶段临时 qpair 与运行期 reactor qpair 的边界；
- 验证多设备 Close/Open 严格语义和故障隔离。

P9M13 退出条件：

- N=2 SPDK 设备组端到端通过；
- 不同 BDF / namespace 不被重复使用；
- 任一设备组 Open 失败时已 attach 资源回滚；
- qpair 不跨 reactor 线程使用；
- Close 能停止所有 reactor 并释放所有 SPDK 资源；
- 同参数重复 Open 通过，不一致配置重复 Open 失败。

## 21. P9M14 - 测试、bench 与文档收敛

目标：

- 完成 P9 测试矩阵；
- 完成 SPDK bench 数据采集和手动归档；
- 完成 P9 收敛文档；
- 同步 README、ROADMAP 和 bench 归档说明；
- 明确 P10 B+树入口。

测试矩阵建议：

| 类型 | 后端 | 说明 |
| --- | --- | --- |
| 普通回归 | sync | 单元测试、TSAN、覆盖率主路径。 |
| 普通回归 | io_uring | 过渡主线和差分验证。 |
| SPDK 只读 | spdk | 有 BDF/nsid 配置时运行 probe 和 namespace 校验。 |
| SPDK 写入 | spdk | 需要显式 `CABE_SPDK_ALLOW_WRITE_TESTS=1`。 |
| SPDK 端到端 | spdk | 需要完整设备组配置和写盘确认。 |

bench 归档建议：

```text
bench/baselines/p9/engine.spdk.gcc.json
bench/baselines/p9/engine_value_put.spdk.gcc.json
bench/baselines/p9/engine_mt.spdk.gcc.json
bench/baselines/p9/wal_concurrency.spdk.gcc.json
```

P9M14 退出条件：

- 全量普通回归通过；
- SPDK 显式设备测试通过；
- SPDK bench JSON 已手动归档；
- 文档明确 VMware 虚拟 NVMe 数据不代表真实 SPDK 性能；
- P9 状态可标记完成；
- P10 B+树阶段入口清晰。

## 22. 总依赖关系

P9 总体按严格串行推进。M0 到 M4 先解决 SPDK 学习曲线、环境、构建、设备和最小封装问题；M5 到 M7 接入 value/data；M8 到 M11 接入 WAL 和 snapshot；M12 到 M14 做全 SPDK 收敛。

```text
P9M0 -> P9M1 -> P9M2 -> P9M3 -> P9M4
      -> P9M5 -> P9M6 -> P9M7
      -> P9M8 -> P9M9
      -> P9M10 -> P9M11
      -> P9M12 -> P9M13 -> P9M14
```

M8 和 M10 在主题上相互独立，但建议仍串行推进。原因是 WAL 和 snapshot 都会触碰按偏移设备抽象、DMA 缓冲区和 Sync 语义，先完成 WAL 能给 snapshot 提供更清晰的落地经验。

## 23. P9 总退出条件

P9 完成时必须满足：

- SPDK 作为 Cabe 自管依赖的路径清晰；
- 普通 `sync` / `io_uring` 构建不依赖 SPDK；
- `CABE_IO_BACKEND=spdk` 可构建；
- SPDK 设备配置使用显式 `BDF + namespace id`；
- Cabe 不自动选择 SPDK 设备；
- SPDK 运行时在 `Engine::Open` 初始化，在 `Engine::Close` 释放；
- qpair 为 reactor 私有资源；
- value/data 路径可以通过 SPDK 完成 `Put/Get/Delete`；
- SPDK 下 `AllocateValueBuffer(key)` 返回 DMA 可用 `ValueBuffer`，正常使用时走零拷贝；
- 应用端自备普通内存在 SPDK 下复制回退；
- WAL 通过 SPDK 完成写入、同步语义判断和恢复；
- snapshot 通过 SPDK 完成 A/B 槽写入、读取、校验和加载；
- 超级块 create/recover 在 SPDK namespace 上完成；
- 全 SPDK 模式不依赖内核块设备路径；
- SPDK 测试默认安全跳过，显式配置后运行；
- SPDK bench 原始 JSON 可归档；
- 文档不对 VMware 虚拟 NVMe 性能作真实性能结论。

## 24. 风险清单

| 风险 | 影响 | 缓解方式 |
| --- | --- | --- |
| SPDK 环境和设备绑定误操作 | 可能误碰系统盘或中断系统设备 | 设备绑定必须显式执行；只按配置 BDF attach；不自动选择设备。 |
| `spdk_env_init` / `spdk_env_fini` 生命周期处理不当 | Close/Open 语义不清或资源泄漏 | P9-D7 锁定严格生命周期；重复 Open 只支持同参数。 |
| qpair 跨线程使用 | 未定义行为或隐蔽数据错误 | qpair 归 reactor 私有；提交和轮询都在所属 reactor 线程。 |
| 多请求在飞过早引入 | 写入状态机、WAL 顺序和 Close drain 复杂度陡增 | P9 初期同步式轮询，多请求在飞留性能阶段。 |
| 普通外部内存误当 DMA 内存 | SPDK I/O 失败或数据错误 | SPDK 下外部普通内存一律复制回退。 |
| WAL Flush 不支持 | 同步 WAL 语义可能不成立 | 按设备能力判断；无易失写缓存时以 completion 为边界，否则同步档失败。 |
| Raw 与 SPDK 设备抽象逻辑漂移 | 两条路径语义不一致 | WAL、snapshot、超级块主逻辑保持一份，Raw 和 SPDK 只做设备适配。 |
| SPDK 错误全部吞成普通 I/O 错误 | 调试困难 | 新增 SPDK 错误码段和上下文日志。 |
| SPDK 测试误写设备 | 数据被破坏 | 写盘测试需要显式危险确认变量。 |
| VMware 虚拟 NVMe 性能误读 | 得出错误优化结论 | bench 只记录原始数据，不做性能优劣结论。 |

## 25. 后续文档编写顺序

接下来建议按以下顺序编写详细设计：

1. `doc/P9/P9M0_spdk_environment_design.md`
2. `doc/P9/P9M1_build_config_design.md`
3. `doc/P9/P9M2_probe_tool_design.md`
4. `doc/P9/P9M3_qpair_dma_rw_verify_design.md`
5. `doc/P9/P9M4_spdk_nvme_device_design.md`
6. `doc/P9/P9M5_superblock_device_view_design.md`
7. `doc/P9/P9M6_value_spdk_working_path_design.md`
8. `doc/P9/P9M7_value_spdk_zero_copy_design.md`
9. `doc/P9/P9M8_wal_device_abstraction_design.md`
10. `doc/P9/P9M9_wal_spdk_design.md`
11. `doc/P9/P9M10_snapshot_device_abstraction_design.md`
12. `doc/P9/P9M11_snapshot_spdk_design.md`
13. `doc/P9/P9M12_full_spdk_e2e_design.md`
14. `doc/P9/P9M13_multidevice_qpair_convergence_design.md`
15. `doc/P9/P9M14_test_bench_convergence_design.md`

每个详细设计文档继续沿用 P8 的讨论方式：

- 先列出决策点和风险点；
- 再逐个讨论并锁定；
- 再编写详细设计；
- 再进行代码实现；
- 最后给出测试命令和退出条件核销。
