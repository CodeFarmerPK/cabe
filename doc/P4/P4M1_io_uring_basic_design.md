# Cabe P4-M1 设计：liburing 接入 + IoUringIoBackend 基础实现

> 本里程碑接入 liburing ≥ 2.9 系统库，实现 `IoUringIoBackend`（满足 P3 定义的
> `IoBackend` C++20 concept），采用最简 submit / wait 同步模型（每次操作提交一个
> SQE → 等待一个 CQE）。功能上与 `SyncIoBackend` 等价，验证 io_uring 路径跑通。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P4 / M1 |
| 状态 | **设计稿** |
| 上游依赖 | P3（IoBackend concept + CMake 分派机制 + SyncIoBackend 参考实现） |
| 下游依赖本里程碑 | P4M2（性能优化——registered buffers + FIXED ops） |
| 退出判定 | 见 §9 |

---

## 1. 目标与范围

### 1.1 目标

1. CMake 接入 liburing ≥ 2.9 系统库（硬性依赖，版本不满足直接报错）。
2. 新建 `io/uring/` 子目录，实现 `IoUringIoBackend`——满足 IoBackend concept 的 5 个方法。
3. CMake 分派生效：`-DCABE_IO_BACKEND=io_uring` 可编译、链接、运行。
4. `engine/backend_config.h` 加 `#elif` 分支，Engine 可透明切换到 io_uring 后端。
5. 编写单元测试（复用 SyncIoBackend 测试结构，需 loop 设备）。

### 1.2 交付范围

1. **`io/uring/io_uring_backend.h` + `io/uring/io_uring_backend.cpp`**：IoUringIoBackend 类。
2. **`io/CMakeLists.txt`**（修改）：条件编译 io_uring 源文件 + 链接 liburing。
3. **`engine/backend_config.h`**（修改）：加 `#elif CABE_USE_IO_URING` 分支。
4. **`engine/CMakeLists.txt`**（修改）：加 `elseif(CABE_IO_BACKEND STREQUAL "io_uring")` 分支。
5. **`test/io/io_uring_backend_test.cpp`**：单元测试。
6. **`test/CMakeLists.txt`**（修改）：注册新测试。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| registered buffers + IOSQE_FIXED_FILE | **P4M2** | M1 先跑通基础路径 |
| TSAN 兼容性处理 | **P4M3** | M1 不处理检测器兼容 |
| 性能基准对比 | **P4M3** | M1 只保证功能正确 |
| 多线程 / 多 ring | **P7** | P4 全程 R=1 单线程 |

---

## 2. 现状盘点

- **IoBackend concept 已定义**（`io/io_backend.h`）：5 个方法——Open / Close / BlockCount / Write / Read。
- **SyncIoBackend 可作参考**：`io/sync/sync_io_backend.*`——同样的方法签名、生命周期模式、错误码约定。
- **CMake 分派机制已就绪**：`engine/backend_config.h` 的 `#if / #elif` 结构 + `engine/CMakeLists.txt` 的 `if / elseif` 结构，加分支即可。
- **liburing 2.9 已安装**：`setup-dev.sh` 包含 `liburing` / `liburing-devel` 安装 + 版本检查。
- **系统环境**：Fedora 43，内核 6.17.8，liburing 2.9，io_uring 未被 sysctl 禁用。

---

## 3. 关键决策（已锁定）

| 编号 | 决策 | 结果 | 理由 |
|---|---|---|---|
| **P4M1-D1** | liburing 接入方式 | 硬性系统依赖（≥ 2.9），CMake `pkg_check_modules` 校验版本，不做源码内嵌或降级 | 与 Fedora 43 / GCC 15 / Clang 20 同级约束；`setup-dev.sh` 已负责安装 |
| **P4M1-D2** | ring 初始化时机 | 与 SyncIoBackend 对称——`Open(path)` 内初始化 ring + 打开设备，`Close()` 内销毁 ring + 关闭设备 | 空构造 → Open 模式一致；ring 生命周期绑定设备 |
| **P4M1-D3** | 队列深度 | P4 内部常量 64；P7 由系统内部自动推算；发版前不暴露到公开 API | 单线程同步模型实际只用 1 个槽位，64 绰绰有余 |
| **P4M1-D4** | O_DIRECT | 保持——`O_RDWR \| O_DIRECT` 打开设备 | 面向裸设备极致性能，与 SyncIoBackend 一致 |
| **P4M1-D5** | 错误码映射 | io_uring CQE 失败时统一返回 `err::kIoBase`，与 SyncIoBackend 一致 | 当前阶段假定硬件正常运行；细粒度错误码留后续扩展 |

---

## 4. IoUringIoBackend 设计

### 4.1 类声明

```cpp
// io/uring/io_uring_backend.h
#ifndef CABE_IO_URING_BACKEND_H
#define CABE_IO_URING_BACKEND_H

#include "io/io_backend.h"
#include "common/error_code.h"
#include "common/structs.h"

#include <cstdint>
#include <liburing.h>
#include <string>

namespace cabe {

    class IoUringIoBackend {
    public:
        IoUringIoBackend() noexcept = default;
        ~IoUringIoBackend();

        IoUringIoBackend(const IoUringIoBackend&) = delete;
        IoUringIoBackend& operator=(const IoUringIoBackend&) = delete;
        IoUringIoBackend(IoUringIoBackend&& other) noexcept;
        IoUringIoBackend& operator=(IoUringIoBackend&& other) noexcept;

        int32_t Open(const std::string& path);
        int32_t Close();
        std::uint64_t BlockCount() const noexcept;
        int32_t Write(std::uint64_t block_idx, const std::byte* buf);
        int32_t Read(std::uint64_t block_idx, std::byte* buf);

        bool is_open() const noexcept;

    private:
        static constexpr unsigned kQueueDepth = 64;

        int fd_ = -1;
        std::uint64_t block_count_ = 0;
        struct io_uring ring_{};
        bool ring_initialized_ = false;
    };

    static_assert(IoBackend<IoUringIoBackend>);

} // namespace cabe

#endif // CABE_IO_URING_BACKEND_H
```

**设计要点**：
- 与 `SyncIoBackend` 相同的方法签名——满足 IoBackend concept。
- `ring_initialized_` 标志控制 ring 的生命周期——`struct io_uring` 本身无法从零值判断是否已初始化。
- `kQueueDepth = 64`：P4 内部常量（D3）。
- move 语义：需支持——DeviceContext 在 `vector::push_back` 中使用。

### 4.2 实现要点

**Open**：
```cpp
int32_t IoUringIoBackend::Open(const std::string& path) {
    if (fd_ >= 0) return err::kIoBase;

    fd_ = ::open(path.c_str(), O_RDWR | O_DIRECT, 0);
    if (fd_ < 0) {
        CABE_LOG_ERROR("IoUringIoBackend::Open 打开设备失败: path=%s", path.c_str());
        return err::kIoBase;
    }

    std::uint64_t dev_bytes = 0;
    if (::ioctl(fd_, BLKGETSIZE64, &dev_bytes) < 0) {
        CABE_LOG_ERROR("ioctl BLKGETSIZE64 失败: fd=%d", fd_);
        ::close(fd_);
        fd_ = -1;
        return err::kIoBase;
    }
    block_count_ = (dev_bytes - kDataRegionOffset) / kValueSize;  // P5：扣除头部 8K 超级块（另有 dev_bytes<=kDataRegionOffset 守卫）
    if (block_count_ == 0) {
        CABE_LOG_ERROR("设备太小: %llu 字节", static_cast<unsigned long long>(dev_bytes));
        ::close(fd_);
        fd_ = -1;
        return err::kEngineInvalidOpts;
    }

    int ret = io_uring_queue_init(kQueueDepth, &ring_, 0);
    if (ret < 0) {
        CABE_LOG_ERROR("io_uring_queue_init 失败: ret=%d", ret);
        ::close(fd_);
        fd_ = -1;
        return err::kIoBase;
    }
    ring_initialized_ = true;

    return err::kSuccess;
}
```

设备打开逻辑与 SyncIoBackend 完全相同（`O_RDWR | O_DIRECT` + `BLKGETSIZE64`），额外加一步 `io_uring_queue_init`。（P5：另加 `io_uring_register_files`；Write/Read 物理偏移加 `kDataRegionOffset`，提交/完成路径加 EINTR 重试 + `user_data` 关联，见 io_uring_backend.cpp。）

**Close**：
```cpp
int32_t IoUringIoBackend::Close() {
    if (fd_ < 0) return err::kSuccess;
    if (ring_initialized_) {
        io_uring_queue_exit(&ring_);
        ring_initialized_ = false;
    }
    ::close(fd_);
    fd_ = -1;
    block_count_ = 0;
    return err::kSuccess;
}
```

先销毁 ring，再关闭设备。幂等——未打开时调用不报错。

**Write**：
```cpp
int32_t IoUringIoBackend::Write(std::uint64_t block_idx, const std::byte* buf) {
    const auto offset = static_cast<__u64>(kDataRegionOffset + block_idx * kValueSize);  // P5：数据区在头部 8K 之后

    struct io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        CABE_LOG_ERROR("io_uring_get_sqe 失败（SQ 满）");
        return err::kIoBase;
    }

    io_uring_prep_write(sqe, fd_, buf, kValueSize, offset);

    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
        CABE_LOG_ERROR("io_uring_submit 失败: ret=%d", ret);
        return err::kIoBase;
    }

    struct io_uring_cqe* cqe = nullptr;
    ret = io_uring_wait_cqe(&ring_, &cqe);
    if (ret < 0) {
        CABE_LOG_ERROR("io_uring_wait_cqe 失败: ret=%d", ret);
        return err::kIoBase;
    }

    int32_t result = err::kSuccess;
    if (cqe->res != static_cast<int>(kValueSize)) {
        CABE_LOG_ERROR("io_uring write 不完整: block_idx=%llu res=%d",
                       static_cast<unsigned long long>(block_idx), cqe->res);
        result = err::kIoBase;
    }

    io_uring_cqe_seen(&ring_, cqe);
    return result;
}
```

最简模型：获取 SQE → 准备写操作 → 提交 → 等待完成 → 检查结果 → 标记已消费。

**Read**：与 Write 对称，将 `io_uring_prep_write` 替换为 `io_uring_prep_read`，检查 `cqe->res == kValueSize`。

**BlockCount / is_open / 析构**：与 SyncIoBackend 完全相同。

**Move 语义**：
```cpp
IoUringIoBackend::IoUringIoBackend(IoUringIoBackend&& other) noexcept
    : fd_(other.fd_)
    , block_count_(other.block_count_)
    , ring_(other.ring_)
    , ring_initialized_(other.ring_initialized_) {
    other.fd_ = -1;
    other.block_count_ = 0;
    other.ring_initialized_ = false;
}
```

按位拷贝 `struct io_uring`（内部是指针和文件描述符），旧对象标记 `ring_initialized_ = false` 防止析构时重复销毁。

---

## 5. CMake 改造

### 5.1 io/CMakeLists.txt

```cmake
# P3-M1：IoBackend 抽象层 + P4-M1：io_uring 后端。
add_library(cabe_io STATIC sync/sync_io_backend.cpp)
target_link_libraries(cabe_io PUBLIC cabe_common)

if(CABE_IO_BACKEND STREQUAL "io_uring")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.9)
    target_sources(cabe_io PRIVATE uring/io_uring_backend.cpp)
    target_link_libraries(cabe_io PUBLIC PkgConfig::LIBURING)
endif()

add_library(cabe::io ALIAS cabe_io)
```

**要点**：
- sync 源文件始终编译（`test_sync_io_backend` 测试需要，且代码量小）。
- io_uring 源文件和 liburing 链接仅在 `CABE_IO_BACKEND=io_uring` 时引入。
- `pkg_check_modules` 校验版本 ≥ 2.9，不满足直接报错。

### 5.2 engine/backend_config.h

```diff
  // ---- IoBackend 分派 ----
  #if defined(CABE_USE_IO_SYNC)
  #include "io/sync/sync_io_backend.h"
  namespace cabe { using IoBackendImpl = SyncIoBackend; }
+ #elif defined(CABE_USE_IO_URING)
+ #include "io/uring/io_uring_backend.h"
+ namespace cabe { using IoBackendImpl = IoUringIoBackend; }
  #else
- #error "未选择 IoBackend：请设置 -DCABE_IO_BACKEND=sync"
+ #error "未选择 IoBackend：请设置 -DCABE_IO_BACKEND=sync 或 io_uring"
  #endif
```

### 5.3 engine/CMakeLists.txt

```diff
  # ---- IoBackend 编译期分派 ----
  if(CABE_IO_BACKEND STREQUAL "sync")
      target_compile_definitions(cabe_engine PUBLIC CABE_USE_IO_SYNC=1)
+ elseif(CABE_IO_BACKEND STREQUAL "io_uring")
+     target_compile_definitions(cabe_engine PUBLIC CABE_USE_IO_URING=1)
  else()
      message(FATAL_ERROR "CABE_IO_BACKEND='${CABE_IO_BACKEND}' 尚未实现")
  endif()
```

---

## 6. 目录结构

```
io/
├── CMakeLists.txt
├── io_backend.h                  # concept（P3M1 已有）
├── sync/                         # P3M1 已有
│   ├── sync_io_backend.h
│   └── sync_io_backend.cpp
└── uring/                        # P4M1 新增
    ├── io_uring_backend.h
    └── io_uring_backend.cpp

test/io/
├── sync_io_backend_test.cpp      # P3M1 已有
└── io_uring_backend_test.cpp     # P4M1 新增
```

---

## 7. 测试设计

### 7.1 测试结构

复用 `SyncIoBackendTest` 的用例结构——两者满足同一个 concept，行为契约相同。

```cpp
class IoUringBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = GetTestDevice();
        if (device_.empty()) GTEST_SKIP() << "CABE_TEST_DEVICE 未设置";
    }
    void TearDown() override { backend_.Close(); }
    cabe::IoUringIoBackend backend_;
    std::string device_;
};
```

### 7.2 用例清单

| 用例 | 验证 |
|---|---|
| `OpenCloseNormal` | Open 成功 → is_open() → Close → !is_open() |
| `DoubleOpenFails` | Open → 再 Open 返回错误 |
| `CloseIdempotent` | 未 Open 就 Close → 不报错 |
| `BlockCountCorrect` | Open → BlockCount() > 0 |
| `WriteReadRoundTrip` | 写 0xAB → 读回比对一致 |
| `WriteReadMultipleBlocks` | 写 4 个不同 pattern → 逐块读回验证 |
| `DestructorAutoCloses` | Open 后直接析构 → 不崩溃 |
| `MoveConstruct` | Open → move → 新对象可用，旧对象无效 |
| `ConceptSatisfied` | `static_assert(IoBackend<IoUringIoBackend>)` |

### 7.3 不需要设备的测试

| 用例 | 验证 |
|---|---|
| `OpenBadPath` | 不存在的路径 → 返回错误 |

### 7.4 test/CMakeLists.txt

```cmake
# P4M1：IoUringIoBackend 测试
add_executable(test_io_uring_backend io/io_uring_backend_test.cpp)
target_link_libraries(test_io_uring_backend PRIVATE cabe::io cabe::engine GTest::gtest_main)
gtest_discover_tests(test_io_uring_backend DISCOVERY_TIMEOUT 60)
```

注：此测试目标始终注册（不区分 `CABE_IO_BACKEND`），直接链接 `cabe::io`（含 io_uring 源文件）。如果 `CABE_IO_BACKEND != io_uring` 导致 io_uring 源文件未编译进 cabe_io，则此测试目标的注册需要条件化——待实装时确认。

---

## 8. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| TSAN 假阳性 | io_uring 的内核态 I/O 完成对 TSAN 不可见 | P4M1 不处理；P4M3 专项解决 |
| `struct io_uring` move 安全性 | 按位拷贝后旧对象 ring 指针悬空 | `ring_initialized_` 标志防止旧对象析构时误销毁 |
| SQ 满（`io_uring_get_sqe` 返回空） | 单线程同步模型下不应发生（深度 64，同时只有 1 个请求在飞） | 仍做防御性检查并返回错误 |
| liburing 系统库缺失 | CMake 配置阶段报错 | `setup-dev.sh` + `pkg_check_modules` 双重保障 |

---

## 9. 退出条件

1. **IoUringIoBackend 实装**：`io/uring/io_uring_backend.*` + `static_assert(IoBackend<IoUringIoBackend>)` 编译通过。
2. **CMake 分派生效**：`-DCABE_IO_BACKEND=io_uring` 配置、编译、链接成功。
3. **单元测试**：10 个用例（需设备 9 个 + 无设备 1 个）全绿。
4. **sync 后端不退步**：`-DCABE_IO_BACKEND=sync` 原有全部测试仍全绿。
5. **Engine 端到端**：`-DCABE_IO_BACKEND=io_uring` 下 `test_engine` 全部用例通过。
6. **覆盖率** ≥ 80%。

---

## 10. 对下游里程碑的接口承诺

| 下游 | 接入点 |
|---|---|
| **P4M2** | 在 `IoUringIoBackend` 基础上加 registered buffers + IOSQE_FIXED_FILE 优化 |
| **P4M3** | TSAN 兼容处理 + 性能基准对比 |
| **P7** | per-reactor 独立 ring + 队列深度自动推算 |

---

**全文完。**
