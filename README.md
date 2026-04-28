# Cabe

一个基于裸块设备的键值存储引擎（C++20）。

> **状态：早期开发中。** 第一个 tag release 之前，API / 磁盘格式 / 错误码都可能发生不兼容变更。

---

## Supported Platform

**Cabe 当前仅支持 Fedora 43。**

这是在当前开发阶段的一个主动选择：

- Cabe 激进使用最新内核能力（`io_uring` 含 `uring_cmd`、`IORING_REGISTER_BUFFERS2`，
  以及计划中的 6.16+ 特性）—— 只有 Fedora 跟进得足够快。
  - 单一目标让有限的维护精力用在特性上，而不是兼容性矩阵上。
  - 代码结构中保留了移植地基（feature gate / fallback 路径），便于未来在不重写的前提下
  重新开放多发行版支持。

| 组件 | 要求 |
|---|---|
| 发行版 | Fedora 43（CMake 强制校验，无例外） |
| 内核 | 6.16+（Fedora 43 默认） |
| 编译器 | GCC 15+ 或 Clang 20+（Fedora 43 默认） |
| CMake | 3.14+ |
| 构建系统 | Ninja（推荐）或 Make |

---

## 设计定位

Cabe 是一个**面向大 value 的 KV 存储引擎**，性能优化目标是
value 大小在 **1 MiB 附近及以上**的场景。

- **"支持"与"保证性能"是两件事**：API 层面对 value 大小没有下限，
  任意 size ≥ 1 字节的 value 都能被正确 Put / Get / Delete；但只有
  ≥ 1 MiB 的 value 能吃到全部优化（硬件 CRC32C、BufferPool 复用、
  O_DIRECT 对齐路径等）。
- **小 value 场景不推荐**：小于 1 MiB 的 value 仍然会占用整个
  1 MiB 定长块（写放大不变），且 QPS 会被 `O_DIRECT + O_SYNC` 延迟
  主导，不适合做高频小 KV 读写。
- **典型适用场景**：对象存储后端、模型权重切片、日志段、大 blob
  缓存等以 MiB/GiB 为单位的数据单元。

---

## Quick Start

### 1. 安装依赖

```bash
./scripts/setup-dev.sh
```

### 2. 准备块设备（必需）

Cabe 直接操作裸块设备,**不接受普通文件**作为 backing。Engine 全程不创建、
不 truncate、不 unlink backing 路径——backing 必须是已存在的块设备节点
（通过 `S_ISBLK` + `BLKGETSIZE64` 校验）。

生产场景直接用 NVMe / SATA 设备(`/dev/nvme0n1` / `/dev/sda`)。
开发 / CI 场景用 loop device 替身：

```bash
sudo ./scripts/mkloop.sh create      # 默认 512 MiB 于 /var/tmp/cabe_test.img
export CABE_TEST_DEVICE=/dev/loop0   # 以 mkloop 输出为准

# bench 需要更大空间(覆盖 BM_Engine_Put 16 MiB 的长 iteration):
# sudo SIZE_MB=16384 ./scripts/mkloop.sh create
# export CABE_BENCH_DEVICE=/dev/loop0
```

### 3. 构建 + 测试

```bash
./scripts/run-tests.sh                # Debug,无 sanitizer
./scripts/run-tests.sh --release      # Release
./scripts/run-tests.sh --asan         # Debug + AddressSanitizer
./scripts/run-tests.sh --tsan         # Debug + ThreadSanitizer
./scripts/run-tests.sh --filter 'IoBackendContract'
```

每个 sanitizer 使用独立 build 目录(`build-asan/` `build-tsan/`),切换不触发全量重编。

未设置 `CABE_TEST_DEVICE` 时 Engine 集成测试自动 SKIP,纯 unit 测试
(util / memory / buffer / freelist)仍会跑。Engine 测试串行执行
(同一裸设备不能被多 test 并发占用)。

### 4. 用完清理 loop device


```bash
sudo ./scripts/mkloop.sh cleanup
```

---

## Project Layout

```
cabe/
├── common/       ChunkId / BlockId / KeyMeta / ChunkMeta，错误码
├── util/         CRC32、时间戳
├── memory/       MetaIndex（key→meta）+ ChunkIndex（chunkId→meta）
├── storage/      FreeList —— 裸设备 BlockId 上限内的块号分配器
├── io/           IoBackend 抽象层(编译期 dispatch) + sync 后端
│                 (P4 加 io_uring 后端,P9 加 SPDK 后端)
│                 设备 I/O + 1 MiB 对齐 buffer 池(原 storage/Storage 与 buffer/BufferPool 合并而来)
├── engine/       Engine —— 顶层门面：Open/Put/Get/Delete/Remove/Close
├── test/         gtest 套件（与模块布局对称）
└── scripts/      开发环境脚本
```


---

## Benchmarks

Cabe ships with a Google Benchmark harness for single-threaded
measurement at the module and engine level.

### Running

```bash
# All benchmarks, terminal output
./scripts/run-bench.sh

# Filter (POSIX regex)
./scripts/run-bench.sh --filter 'CRC32'
./scripts/run-bench.sh --filter 'BM_Engine_Put'

# Archive: generates bench/baselines/LABEL-YYYYMMDD-HHMMSS.json
./scripts/run-bench.sh --archive p1.0-post
```

---

## Roadmap

| 阶段 | 主题 | 状态 |
|---|---|--|
| P-1 | Fedora 43 开发环境 | ✅ 完成 |
| P0  | BufferPool（mmap + O_DIRECT 对齐） | ✅ 完成 |
| P1  | 线程安全（shared_mutex + atomic）+ Google Benchmark 骨架 | ✅ 完成 |
| P2  | C++ API 契约定型(Pimpl + Status)+ 裸设备语义重构 | ✅ 完成 |
| P3  | IoBackend 抽象层（编译期 dispatch，仅 sync 后端） | ✅ 完成 |
| P4  | io_uring 后端 + registered buffer pool（接管 BufferPool） | 🚧 进行中（[设计稿](doc/p4_io_uring_design.md)） |
| P5  | WAL + 崩溃恢复 | 计划 |
| P6  | 多线程 reactor 引擎 | 计划 |
| P7  | 自研 B+ 树 + 细粒度并发 | 计划 |
| P8  | scatter-gather 多 chunk 合并 I/O | 计划 |
| P9  | SPDK 用户态驱动后端（可选） | 不确定 |

---

## Design Documents

详细设计文档位于 `doc/`:

- [P4 io_uring 分阶段实施设计](doc/p4_io_uring_design.md) — 19 项决策(D1–D19)、9 个里程碑(M1–M9)、12 项风险点(R1–R12);v1.0(2026-04-28,讨论稿)

---

## Development Notes

- **本地跑 TSAN/ASAN**：提交前手动 `./scripts/run-tests.sh --tsan / --asan`，作为"准入基线"。
- **新内核特性走 feature gate**：`CABE_HAVE_*` 门控，即使只在 Fedora 43 上测，
  代码结构仍保持可移植形态。
  - **基线归档**：每个 P 结束保存一份 benchmark JSON 到 `bench/baselines/`，下一阶段可检测回归。

---

## License

待定 —— 第一个 tag release 之前确定。