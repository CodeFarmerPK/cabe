# Cabe

定长 value 键值存储引擎,直接操作 NVMe 裸块设备。

## 是什么

Cabe 是一个面向单机部署的 KV 存储引擎:

- **定长 value**:每次 Put / Get 的数据大小**恒为 1 MiB**(`kValueSize`)
- **NVMe 裸设备**:直接打开 `/dev/nvmeXnY`,不走文件系统、不走 page cache
- **一层索引,一次 IO**:`key → BlockId` 一次查找后,数据 IO 直达物理位置
- **WAL + Snapshot**:外置 WAL 设备保证持久化与 crash recovery,周期性 snapshot 截断 WAL
- **多 NVMe 聚合**:支持 N 个 `(data, wal)` 设备对,key hash 路由分布
- **无锁多线程**:reactor 模型 + lock-free 队列,无 mutex 路径
- **同步 API**:`Put` / `Get` / `Delete` 调用同步返回,内部按需异步

## 不是什么

下列能力**不在项目范围内**,且不会在后续版本加入:

- ❌ 大 value 切分 / 变长 value(交由上层)
- ❌ 范围扫描 / 事务 / 二级索引
- ❌ 跨设备原子操作 / 批量 API
- ❌ 多机复制 / 跨平台
- ❌ 运行期变更 N / 数据迁移 / rebalance

## 项目状态

当前处于 **P0(基础设施)阶段**。完整路线见 [ROADMAP.md](ROADMAP.md)。

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 基础设施 | ✅ 完成 |
| P1 | 单线程核心 | ✅ 完成 |
| P2 | API 冻结 + Forward-compat PoC | ⏳ |
| P3 | IoBackend + MetaIndex 抽象 | ⏳ |
| P4 | io_uring 后端 | ⏳ |
| P4.5 | FreeList 改造 | ⏳ |
| P5 | WAL + Recovery + Snapshot | ⏳ |
| P6 | Group Commit | ⏳ |
| P7 | Reactor + 无锁 MT + 多 device 端到端 | ⏳ |
| P8 | 零拷贝主路径 | ⏳ |
| P9 | B+ 树索引(学习路径) | ⏳ |
| P10 | SPDK 后端 | ⏳ |
| P11 | 多 NVMe 规模化 | ⏳ |
| P12 | 可观测性 + 运维工具 | ⏳ |

## 设计原则

1. **专一**:做一件事 —— 定长 value KV,做到极致
2. **简单**:每一层抽象都有具体功能驱动,不为未来留"装饰性接口"
3. **诚实**:不为内核 bug / 设备掉线做应用层兜底;不承诺做不到的事
4. **可观测**:每阶段含明文性能红线;bench 数据归档可追溯
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
  - SPDK(P10+,可选)
  - xxhash 库(P0+,路由 hash)

完整依赖装机脚本:`scripts/setup-dev.sh`(仅 Fedora 43+)。

## 构建

```bash
# 初次环境装配(仅 Fedora 43+)
./scripts/setup-dev.sh

# 配置 + 构建(默认 Release + SyncIoBackend + HashMetaIndex)
cmake -S . -B build -G Ninja
cmake --build build

# Sanitizer 矩阵
cmake -S . -B build-asan  -G Ninja -DCABE_SANITIZER=address
cmake -S . -B build-tsan  -G Ninja -DCABE_SANITIZER=thread
cmake -S . -B build-ubsan -G Ninja -DCABE_SANITIZER=undefined
```

编译期可替换组件(P3+ 起生效):

```bash
# 切换 I/O 后端
cmake -S . -B build -DCABE_IO_BACKEND=sync       # 默认,P3
cmake -S . -B build -DCABE_IO_BACKEND=io_uring   # P4+
cmake -S . -B build -DCABE_IO_BACKEND=spdk       # P10+

# 切换索引实现
cmake -S . -B build -DCABE_META_INDEX=hashmap    # 默认,P3
cmake -S . -B build -DCABE_META_INDEX=bplustree  # P9+
```

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
├── engine/               # Engine / Options / Status(P1 起)
├── io/                   # IoBackend 实现(P3 起)
├── index/                # MetaIndex 实现(P3 起)
├── wal/                  # WAL writer 与 recovery(P5 起)
├── reactor/              # Reactor 与无锁队列(P7 起)
├── doc/                  # 阶段设计稿（doc/PN/PNMn_<主题>_design.md 风格）
├── bench/
│   └── baselines/        # 各阶段 bench 归档
├── scripts/              # 装机与运维脚本
└── test/                 # 单元与集成测试
```

## 文档导航

- [ROADMAP.md](ROADMAP.md) — 完整路线图与架构决策(D1–D26)
- [doc/P0/README.md](doc/P0/README.md) — P0 阶段索引（M1–M7 各里程碑设计稿）
- [doc/P0/P0M7_convergence_design.md](doc/P0/P0M7_convergence_design.md) — P0 阶段收敛稿（薄索引）
- [doc/P1/README.md](doc/P1/README.md) — P1 阶段占位索引（待启动）
- [doc/P2/README.md](doc/P2/README.md) — P2 阶段占位索引（待启动）

## 许可

待定(本项目目前处于早期开发阶段)。