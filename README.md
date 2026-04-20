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

### 2. 构建 + 测试

```bash
./scripts/run-tests.sh                # Debug，无 sanitizer
./scripts/run-tests.sh --release      # Release
./scripts/run-tests.sh --asan         # Debug + AddressSanitizer
./scripts/run-tests.sh --tsan         # Debug + ThreadSanitizer
./scripts/run-tests.sh --filter 'BufferPool'
```

每个 sanitizer 使用独立 build 目录（`build-asan/` `build-tsan/`），切换不触发全量重编。

### 3. 使用真实块设备（可选）

集成测试依赖 `O_DIRECT`，tmpfs / overlayfs 不支持。创建 loop device：

```bash
sudo ./scripts/mkloop.sh create      # 默认 512 MiB 于 /var/tmp/cabe_test.img
export CABE_TEST_DEVICE=/dev/loop0   # （以 mkloop 输出为准）
./scripts/run-tests.sh
sudo ./scripts/mkloop.sh cleanup
```

---

## Project Layout

```
cabe/
├── common/       ChunkId / BlockId / KeyMeta / ChunkMeta，错误码
├── util/         CRC32、时间戳
├── buffer/       BufferPool —— mmap + O_DIRECT 对齐缓冲
├── memory/       MetaIndex（key→meta）+ ChunkIndex（chunkId→meta）
├── storage/      FreeList（块分配）+ Storage（O_DIRECT pread/pwrite）
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
|---|---|---|
| P-1 | Fedora 43 开发环境 | ✅ 完成 |
| P0  | BufferPool（mmap + O_DIRECT 对齐） | ✅ 完成 |
| P1  | 线程安全（shared_mutex + atomic）+ Google Benchmark 骨架 | 🚧 下一步 |
| P2  | C API 契约定型 | 计划 |
| P3  | io_uring 异步 I/O + 异步 API | 计划 |
| P4  | WAL + 崩溃恢复 | 计划 |
| P5  | 多线程 reactor 引擎 | 计划 |
| P6  | 自研 B+ 树 + 细粒度并发 | 计划 |
| P7  | 零拷贝（registered buffers / scatter-gather） | 计划 |
| P8  | SPDK 用户态驱动（可选） | 不确定 |

---

## Development Notes

- **本地跑 TSAN/ASAN**：提交前手动 `./scripts/run-tests.sh --tsan / --asan`，作为"准入基线"。
- **新内核特性走 feature gate**：`CABE_HAVE_*` 门控，即使只在 Fedora 43 上测，
  代码结构仍保持可移植形态。
  - **基线归档**：每个 P 结束保存一份 benchmark JSON 到 `bench/baselines/`，下一阶段可检测回归。

---

## License

待定 —— 第一个 tag release 之前确定。