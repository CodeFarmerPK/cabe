# Cabe

定长 value 键值存储引擎,直接操作 NVMe 裸块设备。

## 是什么

Cabe 是一个面向单机部署的 KV 存储引擎:

- **定长 value**:每次 Put / Get 的数据大小**恒为 1 MiB**(`kValueSize`)
- **NVMe 裸设备**:sync / `io_uring` 后端打开块设备节点；P9 SPDK 后端按 `BDF + namespace id + 可选字节区间` 直接访问 NVMe namespace；均不走文件系统或 page cache
- **一层索引,一次 IO**:`key → BlockId` 一次查找后,数据 IO 直达物理位置
- **WAL + Snapshot**:外置 WAL 设备保证持久化与 crash recovery；阈值触发或显式请求的 snapshot 用于回收 WAL
- **多 NVMe 聚合**:支持 N 个 `(data, WAL, snapshot)` 设备组,key hash 路由分布
- **无锁多线程**:reactor 模型 + lock-free 队列,无 mutex 路径
- **同步 API**:`Put` / `Get` / `Delete` 调用同步返回；内部经 reactor 投递并在本次调用内等待完成

## 不是什么

下列能力**不在项目范围内**,且不会在后续版本加入:

- ❌ 大 value 切分 / 变长 value(交由上层)
- ❌ 范围扫描 / 事务 / 二级索引
- ❌ 跨设备原子操作 / 批量 API
- ❌ 多机复制 / 跨平台
- ❌ 运行期变更 N / 数据迁移 / rebalance

## 项目状态

当前 **P8(零拷贝主路径)已收尾**，**P9M0(SPDK 环境与路线基线)**和 **P9M1(SPDK 构建接入与配置模型)**均已实现，下一步进入 P9M2 定向探测与只读验证工具设计。完整路线见 [ROADMAP.md](ROADMAP.md)。

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 基础设施 | ✅ 完成 |
| P1 | 单线程核心 | ✅ 完成 |
| P2 | API 冻结声明 | ✅ 完成 |
| P3 | IoBackend + MetaIndex 抽象 | ✅ 完成 |
| P4 | io_uring 后端 | ✅ 完成 |
| P4.5 | 块分配器改造 | ✅ 完成 |
| P5 | WAL + Recovery + Snapshot | ✅ 完成 |
| P6 | Group Commit | ✅ 完成 |
| P7 | Reactor + 无锁 MT + 多 device 端到端 | ✅ 完成 |
| P8 | 零拷贝主路径 | ✅ 完成 |
| P9 | SPDK NVMe API 后端接入 | 🚧 M0/M1 已实现，下一步 M2 |
| P10 | 生产级无锁内存 B+树索引 | ⏳ |
| P11 | 多 NVMe 规模化与真盘验证 | ⏳ |
| P12 | 可观测性 + 运维工具 | ⏳ |

## 设计原则

1. **专一**:做一件事 —— 定长 value KV,做到极致
2. **简单**:每一层抽象都有具体功能驱动,不为未来留"装饰性接口"
3. **诚实**:不为内核 bug / 设备掉线做应用层兜底;不承诺做不到的事
4. **可观测**:性能数据归档可追溯；是否比较和设置门槛由阶段设计决定，P9 在虚拟 NVMe 上只保存原始数据、不作性能结论
5. **学习与生产并存**:生产路径走最优工程选择;学习路径(如 B+ 树)隔离在 abstraction 之下

## 环境要求

- **OS**:Fedora 43+(或 Linux 内核 ≥ 6.16)
- **编译器**:GCC 15+ 或 Clang 20+
- **C++ 标准**:C++20
- **依赖**:
  - `liburing` ≥ 2.9(P4+)
  - `gtest` / `gmock`(测试)
  - `google-benchmark`(微基准)
  - `gcovr`(覆盖率报告，M6+ `scripts/run-coverage.sh` 依赖)
  - SPDK(P9M0 起通过 `third_party/spdk` 子模块接入；Cabe 主构建接入从 P9M1 开始)
  - 内嵌 xxhash 源码(`third_party/xxhash`，P0+ 路由 hash，不依赖系统动态库)

Cabe 基础依赖装机脚本为 `scripts/setup-dev.sh`（仅 Fedora 43+）。SPDK 子模块、依赖、编译和环境检查由独立的 `scripts/setup-spdk.sh` 管理；NVMe 设备绑定由 `scripts/spdk-device.sh` 显式管理。

## 构建

```bash
# 初次环境装配(仅 Fedora 43+)
./scripts/setup-dev.sh

# P9M0/P9M1：初始化、安装依赖并编译仓库内锁定版本的 SPDK
./scripts/setup-spdk.sh init
./scripts/setup-spdk.sh deps
./scripts/setup-spdk.sh build --jobs="$(nproc)"  # 成功后生成供 Cabe CMake 校验的构建戳
# check 还会检查 P9M0 的 1 GiB 大页基线；P9M1 无设备构建本身不要求大页
./scripts/setup-spdk.sh check

# 配置 + 构建(Release + HashMetaIndex;自 P6 起 I/O 后端必填、无默认)
cmake -S . -B build -G Ninja -DCABE_IO_BACKEND=io_uring   # 当前可用的过渡后端
cmake --build build

# P9M1：真实静态链接 SPDK/DPDK，但 Engine 数据路径仍明确返回尚未实现
cmake -S . -B build-spdk -G Ninja -DCABE_IO_BACKEND=spdk -DCABE_BUILD_TESTS=ON
cmake --build build-spdk
ctest --test-dir build-spdk --output-on-failure

# Sanitizer 矩阵(后端必填;TSAN 与 io_uring 不兼容,TSAN 用 sync)
cmake -S . -B build-asan  -G Ninja -DCABE_IO_BACKEND=io_uring -DCABE_SANITIZER=address
cmake -S . -B build-tsan  -G Ninja -DCABE_IO_BACKEND=sync     -DCABE_SANITIZER=thread
cmake -S . -B build-ubsan -G Ninja -DCABE_IO_BACKEND=io_uring -DCABE_SANITIZER=undefined
```

编译期可替换组件(P3+ 起生效):

```bash
# 切换 I/O 后端(必填,自 P6 起无默认;见 ROADMAP P6 段「后端策略」/ doc/P6/README.md D10)
cmake -S . -B build -DCABE_IO_BACKEND=io_uring   # 当前过渡后端 + P6 性能锚点
cmake -S . -B build -DCABE_IO_BACKEND=sync       # 仅正确性回归(开发/性能基准已冻结于 P6)
cmake -S . -B build-spdk -DCABE_IO_BACKEND=spdk  # P9M1 构建/配置已可用；P9M6 形成可工作 value/data 后端

# 切换索引实现(后端仍必填)
cmake -S . -B build -DCABE_IO_BACKEND=io_uring -DCABE_META_INDEX=hashmap    # 默认索引,P3
cmake -S . -B build -DCABE_IO_BACKEND=io_uring -DCABE_META_INDEX=bplustree  # P10 计划能力，当前尚不可用
```

> 提示:日常测试走 `scripts/run-tests.sh`，需显式指定 `--backend=sync|io_uring|spdk`；
> P9M1 的 SPDK 测试不要求设备。覆盖率和基准脚本当前仍按各自阶段设计使用 Raw 后端。
> P9M1-D19 尚未裁决前，`run-tests.sh` 会拒绝 `spdk` 与 ASAN、TSAN、UBSAN 的组合。

## 仓库结构

```
cabe/
├── ROADMAP.md            # 完整路线图与架构决策
├── README.md             # 本文件
├── CMakeLists.txt        # 根 CMake(P0 内引入)
├── common/               # 跨模块基础类型与日志
│   ├── error_code.h      # 错误码段位划分
│   ├── logger.h          # 日志宏(P0 内接 stderr 最简实现)
│   └── structs.h         # ValueMeta / BlockId / kValueSize / DataView
├── util/                 # 工具库
│   ├── crc32.{h,cpp}     # CRC32C(SSE4.2 / 软件 fallback)
│   ├── cpu_features.{h,cpp}  # CPU 能力检测
│   ├── hash.{h,cpp}      # xxh3 路由 hash(P0 内引入)
│   └── util.h            # 时间戳等
├── engine/               # Engine / Options / Status / Reactor(P1/P7 起)
├── io/                   # IoBackend 实现(P3 起)
├── index/                # MetaIndex 实现(P3 起)
├── slots/                # BlockAllocator 实现(P4.5 起)
├── snapshot/             # Snapshot 格式与读写(P5 起)
├── wal/                  # WAL writer 与 recovery(P5 起)
├── third_party/
│   ├── spdk/             # P9M0 固定到 v26.01 的 SPDK 子模块
│   └── xxhash/           # 内嵌 xxhash 源码
├── doc/                  # 阶段设计稿（doc/PN/PNMn_<主题>_design.md 风格）
├── bench/
│   └── baselines/        # 性能基线归档(P6 为锚点;见 bench/baselines/README.md)
├── scripts/              # 装机与运维脚本
└── test/                 # 单元与集成测试
```

## 文档导航

- [ROADMAP.md](ROADMAP.md) — 完整路线图与架构决策(D1–D26)
- [doc/P0/README.md](doc/P0/README.md) — P0 阶段索引（M1–M7 各里程碑设计稿）
- [doc/P0/P0M7_convergence_design.md](doc/P0/P0M7_convergence_design.md) — P0 阶段收敛稿（薄索引）
- [doc/P1/README.md](doc/P1/README.md) — P1 已完成阶段索引
- [doc/P2/README.md](doc/P2/README.md) — P2 已完成阶段索引
- [doc/P8/README.md](doc/P8/README.md) — P8 零拷贝总体设计与收敛状态
- [doc/P9/README.md](doc/P9/README.md) — P9 SPDK NVMe API 后端总体里程碑设计
- [doc/P9/P9M0_spdk_environment_design.md](doc/P9/P9M0_spdk_environment_design.md) — P9M0 环境与路线基线
- [doc/P9/P9M1_build_config_design.md](doc/P9/P9M1_build_config_design.md) — P9M1 构建接入与配置模型详细设计

## 许可

待定(本项目目前处于早期开发阶段)。
