# Cabe P1-M2 设计：数据通路基础设施（BufferPool + 朴素 I/O）

> 本里程碑实装 `BufferPool`（4 KiB 对齐的 1 MiB 块池）和朴素 I/O 封装（pwrite / pread +
> O_DIRECT），并新增 `scripts/mkloop.sh` 创建 loop 设备供本地测试。`DeviceContext` 补入
> `BufferPool` 字段，`Engine::Open` 改用 O_DIRECT 打开设备。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P1 / M2 |
| 状态 | **完成稿（待 owner 终审）** —— 五项决策经决策梳理拍板（见 §3） |
| 上游依赖 | P1M1（Engine 骨架 + Options / Status / DeviceContext 就位） |
| 下游依赖本里程碑 | P1M3（FreeList + MetaIndex）、P1M4（Put / Get / Delete 端到端） |
| 关联约束 | ROADMAP P1 范围"朴素 I/O + 朴素 BufferPool"；D24（零拷贝 P8 起） |
| 退出判定 | 见 §11（六条） |

---

## 1. 目标与范围

### 1.1 目标

1. 实装 `BufferPool`：构造时一次 `aligned_alloc` 分配 `kDefaultPoolBlocks × kValueSize` 对齐
   内存（4 KiB 对齐），提供 `Allocate()` / `Free()` 接口。
2. 实装朴素 I/O 辅助函数：`WriteBlock(fd, block_idx, buf)` / `ReadBlock(fd, block_idx, buf)`，
   封装 `pwrite` / `pread` + O_DIRECT 约束。
3. `Engine::Open` 改用 `O_DIRECT` 打开设备 / 文件。
4. `DeviceContext` 补入 `BufferPool pool` 字段。
5. 新增 `scripts/mkloop.sh`（创建 / 清理 / 查询 loop 设备），供本地测试使用。
6. 改造 `engine_test.cpp`：测试文件路径从 `/tmp/`（tmpfs，不支持 O_DIRECT）改为读环境变量
   `CABE_TEST_DEVICE`（loop 设备路径），未设置时 `GTEST_SKIP`。
7. 编写 BufferPool 单元测试 + I/O 读写集成测试。

### 1.2 交付范围（本里程碑产出）

1. **`engine/buffer_pool.h` + `engine/buffer_pool.cpp`**：BufferPool 类。
2. **`engine/io.h` + `engine/io.cpp`**：`WriteBlock` / `ReadBlock` 辅助函数。
3. **`engine/device_context.h` 修改**：补入 `BufferPool pool` 字段。
4. **`engine/engine.cpp` 修改**：`Open` 加 `O_DIRECT` 标志 + 初始化 BufferPool。
5. **`scripts/mkloop.sh`**：loop 设备管理脚本。
6. **`test/engine/engine_test.cpp` 修改**：测试路径改 `CABE_TEST_DEVICE` + GTEST_SKIP。
7. **`test/engine/buffer_pool_test.cpp`**：BufferPool 单元测试。
8. **`test/engine/io_test.cpp`**：I/O 读写集成测试（需 loop 设备）。
9. **`engine/CMakeLists.txt` 修改**：加 `buffer_pool.cpp` / `io.cpp` 源文件。
10. **`test/CMakeLists.txt` 修改**：注册新测试。

### 1.3 推迟范围（明确推迟，标注落点）

| 推迟项 | 落点 | 原因 |
|---|---|---|
| IoBackend 抽象层 | **P3** | P1 不做抽象——直接 pwrite / pread；P3 引入接口后 sync / io_uring / spdk 各自实现 |
| RAII BufferHandle（零拷贝所有权） | **P8** | P1 用 Allocate / Free 原始指针即够；P8 零拷贝才需要所有权转移语义 |
| BufferPool 容量可配 | **P2** | P1 硬编码 16 块；P2 冻结 Options 时按需加 `pool_blocks` 字段 |
| FreeList / MetaIndex | **P1M3** | 本里程碑只做 buffer + I/O 层 |

---

## 2. 现状盘点

- **P1M1 已落地**：`engine/` 目录含 `options.h` / `status.h` / `engine.h` / `engine.cpp` /
  `device_context.h` / `CMakeLists.txt`。`Engine::Open` 用 `::open(path, O_RDWR | O_CREAT, 0600)`
  打开文件——**未加 O_DIRECT**。
- **`DeviceContext` 只含 `int fd`**——需补入 `BufferPool`。
- **测试用 `/tmp/`（tmpfs）**——不支持 O_DIRECT，P1M2 加 O_DIRECT 后会 EINVAL。需改测试环境。
- **P0 已有**：`kValueSize = 1 MiB`、`BlockId::byte_offset()` = `block_idx × kValueSize`——
  I/O 偏移天然对齐。
- **`setup-dev.sh` 已装 `util-linux`**（含 `losetup`）——可直接创建 loop 设备。

---

## 3. 关键决策（owner 已拍板）

| 编号 | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| **P1M2-D1** | 对齐策略：编译期常量 `kPageSize = 4096` | 运行时 `getpagesize()` | Fedora 43 x86_64 页大小固定 4096；运行时探测增加复杂度无收益 | 锁定 |
| **P1M2-D2** | O_DIRECT 协作：所有 I/O 全走 BufferPool 分配的对齐 buffer | 部分小读绕过 pool | cabe 只有 1 MiB 整块 I/O，没有"小读"场景 | 锁定 |
| **P1M2-D3** | 测试环境：loop 设备（128 GiB）+ `scripts/mkloop.sh`；测试读 `CABE_TEST_DEVICE` 环境变量，未设置时 GTEST_SKIP | 文件 + O_DIRECT（tmpfs 不支持）/ 真实 NVMe（需有空闲设备） | loop 设备支持 O_DIRECT + 不需要真实 NVMe + 容量充足（128 GiB = 131072 个 BlockId） | 锁定 |
| **P1M2-D4** | BufferPool API：`Allocate() → std::byte*`（池满返 nullptr）/ `Free(buf)` | RAII BufferHandle（P1 过度）/ 返回 span（无所有权语义） | P1 最简；Engine 在同一函数里 Allocate→I/O→Free，不会忘记归还；P8 再引入 RAII | 锁定 |
| **P1M2-D5** | BufferPool 容量：硬编码 `kDefaultPoolBlocks = 16`（16 块 × 1 MiB = 16 MiB） | Options 可配（P1 Options 还不冻结）/ 按设备大小算（过度） | P1 单线程同时最多用 1-2 块；16 块留足余量；P7 多线程时再调 | 锁定 |

---

## 4. 常量定义

在 `engine/buffer_pool.h` 或独立的 `engine/constants.h`（推荐放 `buffer_pool.h`，避免多一个头文件）：

```cpp
namespace cabe {
    inline constexpr std::size_t kPageSize = 4096;
    inline constexpr std::size_t kDefaultPoolBlocks = 16;
}
```

与 P0 的 `kValueSize`（在 `common/structs.h`）配合：
- 每块 buffer 大小 = `kValueSize`（1 MiB）
- 对齐 = `kPageSize`（4 KiB）
- 总池大小 = `kDefaultPoolBlocks × kValueSize`（16 MiB）

---

## 5. BufferPool 设计

### 5.1 类声明

```cpp
// engine/buffer_pool.h
#ifndef CABE_BUFFER_POOL_H
#define CABE_BUFFER_POOL_H

#include "common/structs.h"

#include <cstddef>
#include <vector>

namespace cabe {

    inline constexpr std::size_t kPageSize = 4096;
    inline constexpr std::size_t kDefaultPoolBlocks = 16;

    class BufferPool {
    public:
        explicit BufferPool(std::size_t block_count = kDefaultPoolBlocks);
        ~BufferPool();

        BufferPool(const BufferPool&) = delete;
        BufferPool& operator=(const BufferPool&) = delete;
        BufferPool(BufferPool&& other) noexcept;
        BufferPool& operator=(BufferPool&& other) noexcept;

        std::byte* Allocate();
        void Free(std::byte* buf);

        std::size_t available() const noexcept;
        std::size_t capacity() const noexcept;

    private:
        std::byte* base_ = nullptr;          // aligned_alloc 分配的整块基地址
        std::size_t block_count_ = 0;        // 总块数
        std::vector<std::byte*> free_list_;  // 空闲块指针栈（LIFO）
    };

} // namespace cabe

#endif // CABE_BUFFER_POOL_H
```

### 5.2 实现要点

**构造**：
```cpp
BufferPool::BufferPool(std::size_t block_count)
    : block_count_(block_count) {
    base_ = static_cast<std::byte*>(
        std::aligned_alloc(kPageSize, block_count * kValueSize));
    if (!base_) {
        CABE_LOG_FATAL("BufferPool: aligned_alloc(%zu, %zu) 失败",
                       kPageSize, block_count * kValueSize);
        std::abort();
    }
    free_list_.reserve(block_count);
    for (std::size_t i = 0; i < block_count; ++i) {
        free_list_.push_back(base_ + i * kValueSize);
    }
}
```

- `aligned_alloc(kPageSize, block_count * kValueSize)`：一次分配整块，保证每块首地址
  按 `kPageSize` 对齐（因为 `kValueSize = 1 MiB` 是 `kPageSize = 4 KiB` 的整数倍，所以
  `base_ + i * kValueSize` 也按 `kPageSize` 对齐）。
- 分配失败 → `CABE_LOG_FATAL` + `std::abort()`（系统级故障，不用错误码——16 MiB 都分配不到
  说明机器状态已经不正常）。

**析构**：
```cpp
BufferPool::~BufferPool() {
    if (base_) {
        if (free_list_.size() != block_count_) {
            CABE_LOG_WARN("BufferPool 析构时仍有 %zu 块未归还",
                          block_count_ - free_list_.size());
        }
        std::free(base_);
        base_ = nullptr;
    }
}
```

**Allocate / Free**：
```cpp
std::byte* BufferPool::Allocate() {
    if (free_list_.empty()) return nullptr;
    std::byte* buf = free_list_.back();
    free_list_.pop_back();
    return buf;
}

void BufferPool::Free(std::byte* buf) {
    free_list_.push_back(buf);
}
```

- `Allocate()` 池满返回 `nullptr`——调用方（Engine）检查后返回错误码（不 abort）。
- `Free()` 不做越界校验（P1 信任内部代码——设计原则"只在系统边界做校验"）。
- P7 多线程时这两个方法需加锁或换无锁结构——P1 单线程不需要。

**Move 语义**：
```cpp
BufferPool::BufferPool(BufferPool&& other) noexcept
    : base_(other.base_)
    , block_count_(other.block_count_)
    , free_list_(std::move(other.free_list_)) {
    other.base_ = nullptr;
    other.block_count_ = 0;
}
```

需要 move 是因为 `DeviceContext` 含 `BufferPool` 字段，而 `DeviceContext` 要能 `push_back`
进 `std::vector`——需要 move 构造。

---

## 6. 朴素 I/O 设计

### 6.1 接口

```cpp
// engine/io.h
#ifndef CABE_IO_H
#define CABE_IO_H

#include "engine/status.h"
#include "common/structs.h"

#include <cstddef>

namespace cabe {

    // 写一整块 kValueSize 数据到 fd 的 block_idx 位置。
    // buf 必须按 kPageSize 对齐（由 BufferPool 保证）。
    Status WriteBlock(int fd, std::uint64_t block_idx, const std::byte* buf);

    // 从 fd 的 block_idx 位置读一整块 kValueSize 数据到 buf。
    // buf 必须按 kPageSize 对齐（由 BufferPool 保证）。
    Status ReadBlock(int fd, std::uint64_t block_idx, std::byte* buf);

} // namespace cabe

#endif // CABE_IO_H
```

### 6.2 实现

```cpp
// engine/io.cpp
#include "engine/io.h"
#include "common/logger.h"

#include <unistd.h>

namespace cabe {

    Status WriteBlock(int fd, std::uint64_t block_idx, const std::byte* buf) {
        const off_t offset = static_cast<off_t>(block_idx * kValueSize);
        ssize_t written = ::pwrite(fd, buf, kValueSize, offset);
        if (written != static_cast<ssize_t>(kValueSize)) {
            CABE_LOG_ERROR("pwrite 失败: fd=%d block_idx=%llu written=%zd",
                           fd, static_cast<unsigned long long>(block_idx), written);
            return Status::Error(err::kIoBase);
        }
        return Status::Ok();
    }

    Status ReadBlock(int fd, std::uint64_t block_idx, std::byte* buf) {
        const off_t offset = static_cast<off_t>(block_idx * kValueSize);
        ssize_t nread = ::pread(fd, buf, kValueSize, offset);
        if (nread != static_cast<ssize_t>(kValueSize)) {
            CABE_LOG_ERROR("pread 失败: fd=%d block_idx=%llu nread=%zd",
                           fd, static_cast<unsigned long long>(block_idx), nread);
            return Status::Error(err::kIoBase);
        }
        return Status::Ok();
    }

} // namespace cabe
```

**设计要点**：
- 每次读写恰好 `kValueSize`（1 MiB）——不做 short read / short write 重试（O_DIRECT 对块设备
  / loop 设备不会 short write；如果真的 short → 硬件或文件系统级故障，返回错误码）。
- `offset = block_idx × kValueSize`——与 `BlockId::byte_offset()` 一致。
- 错误码用 `err::kIoBase`（io 段第 0 号，-101000）；P3 IoBackend 抽象层时细化为
  `kIoWriteFailed` / `kIoReadFailed` 等具体码。
- 不做 `buf` 对齐校验——内部代码，由 BufferPool 保证（设计原则"只在系统边界做校验"）。

---

## 7. Engine::Open 改造

### 7.1 O_DIRECT 标志

```cpp
int fd = ::open(cfg.path.c_str(), O_RDWR | O_DIRECT, 0);
```

改动点：
- 加 `O_DIRECT`——绕过 page cache，直接到设备。
- 去掉 `O_CREAT`——loop 设备是 `/dev/loopN` 块设备节点，不需要创建；`mkloop.sh` 已经准备好设备。
- 权限参数改 `0`（块设备节点已有权限，不需要 mode 参数）。

### 7.2 初始化 BufferPool

```cpp
for (const auto& cfg : opts.devices) {
    int fd = ::open(cfg.path.c_str(), O_RDWR | O_DIRECT, 0);
    if (fd < 0) { ... }
    DeviceContext dc;
    dc.fd = fd;
    dc.pool = BufferPool(kDefaultPoolBlocks);
    devices_.push_back(std::move(dc));
}
```

`DeviceContext` 改为：
```cpp
struct DeviceContext {
    int fd = -1;
    BufferPool pool;
};
```

因为 `BufferPool` 支持 move 构造（§5.2），`push_back(std::move(dc))` 可正常工作。

---

## 8. `scripts/mkloop.sh` 设计

参考 owner 提供的参考脚本，适配 P1M2：

```
用法:
  ./scripts/mkloop.sh create    # 创建 loop 设备（128 GiB）
  ./scripts/mkloop.sh cleanup   # 卸载 + 删除镜像文件
  ./scripts/mkloop.sh status    # 查看当前状态

环境变量可覆盖:
  SIZE_MB    镜像大小，默认 131072（128 GiB）
  IMG_PATH   镜像文件路径，默认 /var/tmp/cabe_test.img
```

**实现要点**：
- `fallocate -l ${SIZE_MB}M` 创建稀疏文件（磁盘按需占用，不是真写 128 GiB）。
- `losetup --find --show` 自动分配 loop 设备编号。
- `create` 输出 `export CABE_TEST_DEVICE=/dev/loopN` 供测试使用。
- 需要 root / sudo。
- 幂等：已存在时复用已有 loop 设备。

---

## 9. 测试设计

### 9.1 测试环境改造（`engine_test.cpp`）

```cpp
namespace {
    std::string GetTestDevice() {
        const char* dev = std::getenv("CABE_TEST_DEVICE");
        return dev ? std::string(dev) : "";
    }
}

class EngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = GetTestDevice();
        if (device_.empty()) GTEST_SKIP() << "CABE_TEST_DEVICE 未设置";
    }
    std::string device_;
};
```

所有需要 I/O 的 Engine 测试用 `CABE_TEST_DEVICE`；纯状态机测试（Open 合法性 / Close /
重复 Open）仍可不依赖设备——但 P1M2 加 O_DIRECT 后 `Open` 必须打开真实块设备，所以
所有涉及 `Open` 的测试都需要设备。

### 9.2 BufferPool 单元测试（`buffer_pool_test.cpp`）

不需要设备——纯内存操作：

| 用例 | 验证 |
|---|---|
| `AllocateAndFree` | 分配一块 → 非 nullptr → Free → 再 Allocate 返回同一指针 |
| `AllocateAll` | 连续分配 `kDefaultPoolBlocks` 块 → 全部非 nullptr → 第 17 块返回 nullptr |
| `FreeRestoresAvailable` | 分配 N 块 → `available()` 减少 → Free → `available()` 恢复 |
| `Alignment` | 分配一块 → 地址按 `kPageSize` 对齐（`reinterpret_cast<uintptr_t>(buf) % kPageSize == 0`） |
| `MoveConstruct` | move 构造后原对象 `capacity() == 0`，新对象正常 |
| `DestructorWarnsOnLeak` | 分配不 Free 直接析构 → 不崩溃（CABE_LOG_WARN 触发但不 abort） |

### 9.3 I/O 集成测试（`io_test.cpp`）

需要 `CABE_TEST_DEVICE`：

| 用例 | 验证 |
|---|---|
| `WriteReadRoundTrip` | BufferPool 分配 → 填充 pattern → `WriteBlock(fd, 0, buf)` → `ReadBlock(fd, 0, out)` → 逐字节比对 |
| `WriteReadMultipleBlocks` | 写 block 0-3 不同 pattern → 逐块读回验证——确认偏移计算正确 |
| `ReadUnwrittenBlock` | 读一个从未写过的 block → `pread` 成功但内容全零（块设备特性） |

---

## 10. CMake 改动

**`engine/CMakeLists.txt`**：
```cmake
add_library(cabe_engine STATIC
    engine.cpp
    buffer_pool.cpp
    io.cpp
)
target_link_libraries(cabe_engine PUBLIC cabe_util)
add_library(cabe::engine ALIAS cabe_engine)
```

**`test/CMakeLists.txt`**：
```cmake
# P1M2 新增
add_executable(test_buffer_pool engine/buffer_pool_test.cpp)
target_link_libraries(test_buffer_pool PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_buffer_pool DISCOVERY_TIMEOUT 60)

add_executable(test_io engine/io_test.cpp)
target_link_libraries(test_io PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_io DISCOVERY_TIMEOUT 60)
```

---

## 11. 退出条件与验证步骤

### 11.1 退出条件

1. **BufferPool 实装**：`engine/buffer_pool.{h,cpp}` 就位；`Allocate` / `Free` / `available` /
   `capacity` + move 语义 + `static_assert` 对齐守护。
2. **I/O 实装**：`engine/io.{h,cpp}` 就位；`WriteBlock` / `ReadBlock` + O_DIRECT + pwrite / pread。
3. **Engine::Open 改造**：加 `O_DIRECT` + 初始化 BufferPool。
4. **mkloop.sh 就位**：`scripts/mkloop.sh` create / cleanup / status 可用。
5. **四档全绿**：`run-tests.sh` 分别 `--asan` / `--tsan` / `--ubsan` / `--release` 跑通（需
   先 `export CABE_TEST_DEVICE=/dev/loopN`；BufferPool 纯内存测试不依赖设备）。
6. **覆盖率**：`run-coverage.sh --strict` ≥ 80%。

### 11.2 验证步骤

```bash
# 1. 创建 loop 设备
sudo ./scripts/mkloop.sh create
export CABE_TEST_DEVICE=/dev/loopN   # 按 mkloop 输出的设备名

# 2. 快速验证（默认 Debug）
./scripts/run-tests.sh

# 3. 四档回归
./scripts/run-tests.sh --asan
./scripts/run-tests.sh --tsan
./scripts/run-tests.sh --ubsan
./scripts/run-tests.sh --release

# 4. clang++ 交叉验证（可选）
./scripts/run-tests.sh --compiler=clang++

# 5. 覆盖率
./scripts/run-coverage.sh --strict

# 6. 清理 loop 设备
sudo ./scripts/mkloop.sh cleanup
```

---

## 12. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| loop 设备需要 root / sudo | `losetup` 需要特权；CI 容器可能受限 | `mkloop.sh` 用 `$SUDO` 自适应；CI 暂未接入（M6-D1 推迟）；本地开发时 sudo 可接受 |
| O_DIRECT 对 tmpfs 不兼容 | `/tmp/` 通常是 tmpfs——P1M1 的测试路径不再可用 | 改用 `CABE_TEST_DEVICE` + GTEST_SKIP；BufferPool 纯内存测试不需要设备 |
| BufferPool 分配失败 abort | 系统级故障（16 MiB 都分不出） | `CABE_LOG_FATAL` + `abort()`——比返回错误码更诚实（"分配不到内存继续跑不下去"） |
| `Free()` 无越界校验 | 传入非 pool 分配的指针 → 未定义行为 | P1 信任内部代码；P7 多线程时加 debug assert |
| 128 GiB loop 设备镜像磁盘占用 | `fallocate` 创建稀疏文件——磁盘按需占用；但 P1M4 端到端写大量数据后可能占满 | `/var/tmp/` 通常在根分区；用户可通过 `IMG_PATH` 指定别的位置 |
| short write / short read | O_DIRECT 对块设备不产生 short write；但如果设备故障或文件系统层出错可能 | 返回 `err::kIoBase` 错误码——P1 不重试 |

---

## 13. 对下游里程碑的接口承诺

| 下游 | 本里程碑提供的接入点 |
|---|---|
| **P1M3** | `BufferPool::Allocate()` 分配对齐 buffer → 供 FreeList / MetaIndex 做 I/O 操作用；`WriteBlock` / `ReadBlock` 供 Put / Get 路径调用 |
| **P1M4** | 端到端路径：`pool.Allocate()` → 填 value → `WriteBlock(fd, block_idx, buf)` → `pool.Free(buf)` 完整链路可用 |
| **P3** | `WriteBlock` / `ReadBlock` 可直接被 `IoBackend::Write` / `IoBackend::Read` 替代——接口签名一致（fd + block_idx + buf）；BufferPool 独立于 IoBackend，不受抽象层改造影响 |
| **P8** | `BufferPool` 的 `Allocate / Free` 可被 `BufferHandle` RAII 包装替代——接口扩展而非重写 |

---

## 14. 与 ROADMAP 一致性核对

| ROADMAP P1 字面 | 本设计实现 | 状态 |
|---|---|---|
| 朴素 I/O（syscall + O_DIRECT，不抽象） | §6 `WriteBlock` / `ReadBlock` + `pwrite` / `pread` + O_DIRECT | ✅ |
| 朴素 BufferPool（对齐到 4 KiB 的 1 MiB 块池） | §5 BufferPool（`kPageSize = 4096`，`kValueSize = 1 MiB`） | ✅ |

---

**全文完。**
