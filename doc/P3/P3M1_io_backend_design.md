# Cabe P3-M1 设计：IoBackend 抽象层

> 本里程碑定义 `IoBackend` C++20 concept（同步接口：Open / Close / BlockCount / Write / Read），
> 实装 `SyncIoBackend`（包装 P1 的 pwrite / pread + O_DIRECT），并把 P1 散在 Engine::Open 里的
> 设备打开 / 关闭 / 大小查询逻辑迁入 SyncIoBackend。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P3 / M1 |
| 状态 | **✅ 已锁定（P3M4 收敛）** |
| 上游依赖 | P1（engine/io.h WriteBlock / ReadBlock）、P2（API 冻结声明） |
| 下游依赖本里程碑 | P3M2（MetaIndex 抽象层）、P3M3（Engine 切换——用 IoBackend concept 替代直接调 WriteBlock） |
| 退出判定 | 见 §9 |

---

## 1. 目标与范围

### 1.1 目标

1. 定义 `IoBackend` C++20 concept：5 个同步方法（Open / Close / BlockCount / Write / Read）。
2. 实装 `SyncIoBackend`：把 P1 的 `::open(O_DIRECT)` + `ioctl(BLKGETSIZE64)` + `pwrite` / `pread` + `::close` 封装到一个类里。
3. 建立 `io/` 目录结构（concept 在顶层、实现在子目录）。
4. 编写 SyncIoBackend 单元测试（需 loop 设备）。
5. P1 的 `engine/io.h` / `engine/io.cpp`（裸函数 WriteBlock / ReadBlock）在 P3M3 Engine 切换后可删除——本里程碑先保留（并行存在）。

### 1.2 交付范围

1. **`io/io_backend.h`**：IoBackend concept 定义。
2. **`io/sync/sync_io_backend.h` + `io/sync/sync_io_backend.cpp`**：SyncIoBackend 类。
3. **`io/CMakeLists.txt`**：io 模块 CMake。
4. **根 `CMakeLists.txt` 修改**：加 `add_subdirectory(io)`。
5. **`test/io/sync_io_backend_test.cpp`**：单元测试（需 CABE_TEST_DEVICE）。
6. **`test/CMakeLists.txt` 修改**：注册新测试。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| Engine 切换到 IoBackend | **P3M3** | 本里程碑只做 concept + 实现；Engine 还用旧的 WriteBlock |
| io_uring 后端 | **P4** | P3 只做 SyncIoBackend |
| SPDK 后端 | **P10** | 同上 |
| 删除 engine/io.h / io.cpp | **P3M3** | Engine 切换后旧裸函数不再需要 |

---

## 2. 现状盘点

- **P1 的 I/O 逻辑分散在两处**：
  - `engine/io.cpp`：`WriteBlock(fd, block_idx, buf)` / `ReadBlock(fd, block_idx, buf)`——只做 pwrite / pread
  - `engine/engine.cpp` Engine::Open：`::open(path, O_RDWR | O_DIRECT)` + `ioctl(BLKGETSIZE64)` + `::close(fd)`——设备生命周期
- **DeviceContext 持有裸 `int fd`**——P3M3 会改为持有 IoBackend 实例
- **`io/` 目录不存在**——本里程碑新建

---

## 3. 关键决策（owner 已拍板）

| 编号 | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| **P3M1-D1** | IoBackend concept 含 Open / Close / BlockCount / Write / Read 五个方法——管理完整设备生命周期 | 只含 Write / Read（Open / Close 留 Engine） | 不同后端打开设备方式不同；Engine 不应知道后端类型 | 锁定 |
| **P3M1-D2** | SyncIoBackend 空构造 + Open 方法（与 Engine 状态机一致） | 构造时打开（无法返回错误码） | Open 返回 int32_t 错误码；与 Engine 空构造 → Open 模式对称 | 锁定 |
| **P3M1-D3** | 目录：`io/io_backend.h`（concept）+ `io/sync/sync_io_backend.*`（实现子目录） | 全放 engine/ / 平铺 io/ | 子目录分层：P4 加 `io/io_uring/`、P10 加 `io/spdk/`——层次清晰 | 锁定 |

---

## 4. IoBackend concept 定义

```cpp
// io/io_backend.h
#ifndef CABE_IO_BACKEND_H
#define CABE_IO_BACKEND_H

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cabe {

    template<typename T>
    concept IoBackend = requires(T& io, const std::string& path,
                                 std::uint64_t block_idx,
                                 const std::byte* wbuf, std::byte* rbuf) {
        { io.Open(path) } -> std::same_as<int32_t>;
        { io.Close() } -> std::same_as<int32_t>;
        { io.BlockCount() } -> std::convertible_to<std::uint64_t>;
        { io.Write(block_idx, wbuf) } -> std::same_as<int32_t>;
        { io.Read(block_idx, rbuf) } -> std::same_as<int32_t>;
    };

} // namespace cabe

#endif // CABE_IO_BACKEND_H
```

**设计要点**：
- 全部返回 `int32_t` 错误码（按返回值分层约定——IoBackend 是内部组件）。
- `BlockCount()` 用 `std::convertible_to`（允许返回 `uint64_t` 或 `size_t`）。
- 无异步方法（D1 锁定——同步接口）。
- 无 BufferHandle 参数（D2 推到 P8——继续用裸 `byte*`）。

---

## 5. SyncIoBackend 设计

### 5.1 类声明

```cpp
// io/sync/sync_io_backend.h
#ifndef CABE_SYNC_IO_BACKEND_H
#define CABE_SYNC_IO_BACKEND_H

#include "io/io_backend.h"
#include "common/error_code.h"
#include "common/structs.h"

#include <cstdint>
#include <string>

namespace cabe {

    class SyncIoBackend {
    public:
        SyncIoBackend() noexcept = default;
        ~SyncIoBackend();

        SyncIoBackend(const SyncIoBackend&) = delete;
        SyncIoBackend& operator=(const SyncIoBackend&) = delete;
        SyncIoBackend(SyncIoBackend&& other) noexcept;
        SyncIoBackend& operator=(SyncIoBackend&& other) noexcept;

        int32_t Open(const std::string& path);
        int32_t Close();
        std::uint64_t BlockCount() const noexcept;
        int32_t Write(std::uint64_t block_idx, const std::byte* buf);
        int32_t Read(std::uint64_t block_idx, std::byte* buf);

        bool is_open() const noexcept;

    private:
        int fd_ = -1;
        std::uint64_t block_count_ = 0;
    };

    // 编译期验证 SyncIoBackend 满足 IoBackend concept
    static_assert(IoBackend<SyncIoBackend>);

} // namespace cabe

#endif // CABE_SYNC_IO_BACKEND_H
```

### 5.2 实现要点

**Open**（从 Engine::Open 迁入）：
```cpp
int32_t SyncIoBackend::Open(const std::string& path) {
    if (fd_ >= 0) return err::kIoBase;  // 已打开

    fd_ = ::open(path.c_str(), O_RDWR | O_DIRECT, 0);
    if (fd_ < 0) {
        CABE_LOG_ERROR("SyncIoBackend::Open 失败: path=%s", path.c_str());
        return err::kIoBase;
    }

    std::uint64_t dev_bytes = 0;
    if (::ioctl(fd_, BLKGETSIZE64, &dev_bytes) < 0) {
        CABE_LOG_ERROR("ioctl BLKGETSIZE64 失败: fd=%d", fd_);
        ::close(fd_);
        fd_ = -1;
        return err::kIoBase;
    }
    block_count_ = (dev_bytes - kDataRegionOffset) / kValueSize;  // P5：扣除头部 8K 超级块（另有 dev_bytes<=kDataRegionOffset 守卫防下溢）
    if (block_count_ == 0) {
        CABE_LOG_ERROR("设备太小: %llu 字节", (unsigned long long)dev_bytes);
        ::close(fd_);
        fd_ = -1;
        return err::kEngineInvalidOpts;
    }
    return err::kSuccess;
}
```

**Close**：
```cpp
int32_t SyncIoBackend::Close() {
    if (fd_ < 0) return err::kSuccess;  // 幂等——未打开也不报错
    ::close(fd_);
    fd_ = -1;
    block_count_ = 0;
    return err::kSuccess;
}
```

**Write / Read**（从 P1 io.cpp 迁入）：
```cpp
int32_t SyncIoBackend::Write(std::uint64_t block_idx, const std::byte* buf) {
    const auto offset = static_cast<off_t>(kDataRegionOffset + block_idx * kValueSize);  // P5：数据区在头部 8K 之后（另有 block_idx 越界守卫）
    ssize_t written = ::pwrite(fd_, buf, kValueSize, offset);
    if (written != static_cast<ssize_t>(kValueSize)) {
        CABE_LOG_ERROR("pwrite 失败: fd=%d block_idx=%llu", fd_, (unsigned long long)block_idx);
        return err::kIoBase;
    }
    return err::kSuccess;
}
```

Read 同理。

**BlockCount**：
```cpp
std::uint64_t SyncIoBackend::BlockCount() const noexcept {
    return block_count_;
}
```

**析构**：
```cpp
SyncIoBackend::~SyncIoBackend() {
    if (fd_ >= 0) {
        CABE_LOG_WARN("SyncIoBackend 析构时仍 Open，自动 Close");
        Close();
    }
}
```

**Move 语义**：需支持——P3M3 DeviceContext 持有 IoBackend 实例需要 push_back 进 vector。

---

## 6. 目录与 CMake

### 6.1 目录结构

```
io/
├── CMakeLists.txt          # io 模块
├── io_backend.h            # concept 定义
└── sync/
    ├── sync_io_backend.h   # SyncIoBackend 声明
    └── sync_io_backend.cpp # SyncIoBackend 实现

test/io/
└── sync_io_backend_test.cpp
```

### 6.2 CMake

**`io/CMakeLists.txt`**：
```cmake
# P3-M1：IoBackend 抽象层。
add_library(cabe_io STATIC sync/sync_io_backend.cpp)
target_link_libraries(cabe_io PUBLIC cabe_common)
target_include_directories(cabe_io PUBLIC ${PROJECT_SOURCE_DIR})
add_library(cabe::io ALIAS cabe_io)
```

**根 `CMakeLists.txt`**：加 `add_subdirectory(io)`（在 `add_subdirectory(engine)` 之后）。

**`test/CMakeLists.txt`**：
```cmake
add_executable(test_sync_io_backend io/sync_io_backend_test.cpp)
target_link_libraries(test_sync_io_backend PRIVATE cabe::io GTest::gtest_main)
gtest_discover_tests(test_sync_io_backend DISCOVERY_TIMEOUT 60)
```

### 6.3 依赖链

```
cabe_flags (INTERFACE)
  └─► cabe_common (INTERFACE)
        ├─► cabe_util (STATIC)
        │     └─► cabe_engine (STATIC)
        └─► cabe_io (STATIC)         ← P3M1 新增
              └─► test_sync_io_backend (TEST)
```

`cabe_io` 不依赖 `cabe_engine`——IoBackend 是独立模块。P3M3 Engine 切换时 `cabe_engine` 链接 `cabe_io`。

---

## 7. 测试设计

### 7.1 SyncIoBackend 单元测试（需 CABE_TEST_DEVICE）

| 用例 | 验证 |
|---|---|
| `OpenCloseNormal` | Open 成功 → is_open() == true → Close → is_open() == false |
| `DoubleOpenFails` | Open → 再 Open 返回错误 |
| `CloseIdempotent` | 未 Open 就 Close → 不报错（幂等） |
| `BlockCountCorrect` | Open → BlockCount() == (dev_bytes - kDataRegionOffset) / kValueSize（P5） |
| `WriteReadRoundTrip` | 分配对齐 buffer → 填 pattern → Write → Read → 比对一致 |
| `WriteReadMultipleBlocks` | 写 block 0-3 不同 pattern → 逐块读回验证 |
| `DestructorAutoCloses` | Open 后直接析构 → 不崩溃（日志警告） |
| `MoveConstruct` | Open → move → 新对象 is_open()，旧对象 !is_open() |
| `ConceptSatisfied` | `static_assert(IoBackend<SyncIoBackend>)` 编译通过（已在头文件） |

### 7.2 不需要设备的测试

| 用例 | 验证 |
|---|---|
| `OpenBadPath` | Open 一个不存在的路径 → 返回 kIoBase |

---

## 8. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| concept 方法签名 P4 不够用 | P4 io_uring 可能需要额外方法（如 RegisterBuffers） | concept 可扩展——冻结是相对的 |
| SyncIoBackend 与 P1 io.cpp 并行存在 | P3M1 不删旧代码——两套 I/O 路径并存 | P3M3 Engine 切换后删旧的 |
| cabe_io 独立于 cabe_engine | 依赖链多一层 | P3M3 链接时 engine → io 一行 |

---

## 9. 退出条件

1. **concept 定义就位**：`io/io_backend.h` 含 5 个方法的 concept。
2. **SyncIoBackend 实装**：`io/sync/sync_io_backend.*` 编译通过 + `static_assert(IoBackend<SyncIoBackend>)` 通过。
3. **单元测试**：9 + 1 = 10 个用例（需设备 9 个 + 无设备 1 个）全绿。
4. **不影响现有测试**：原 65 个用例不退步。
5. **四档全绿**：run-tests.sh --asan / --tsan / --ubsan / --release。
6. **覆盖率** ≥ 80%。

---

## 10. 对下游里程碑的接口承诺

| 下游 | 本里程碑提供的接入点 |
|---|---|
| **P3M2** | IoBackend concept 定义可作为 MetaIndex concept 设计的参考模板 |
| **P3M3** | `SyncIoBackend` 可直接替换 DeviceContext 里的 `int fd` + Engine 里的 WriteBlock / ReadBlock 调用 |
| **P4** | `io/io_uring/` 子目录下新增 `IoUringBackend` 实现同一 concept |
| **P10** | `io/spdk/` 子目录下新增 `SpdkBackend` 实现同一 concept |

---

**全文完。**
