# Cabe P3-M3 设计：Engine 切换 + CMake 分派

> 本里程碑将 Engine 从直接操作裸文件描述符 + P1 MetaIndex 切换到通过 P3M1/P3M2 定义的
> IoBackend / MetaIndexBackend 抽象层调用，并让 CMake `CABE_IO_BACKEND` / `CABE_META_INDEX`
> 变量真正控制编译期后端选择。切换后功能等价于 P2，公开 API 不变。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P3 / M3 |
| 状态 | **设计稿** |
| 上游依赖 | P3M1（IoBackend + SyncIoBackend）、P3M2（MetaIndexBackend + HashMetaIndex） |
| 下游依赖本里程碑 | P3M4（收敛）、P4（io_uring 后端——加 `elseif` 分支即可接入） |
| 退出判定 | 见 §10 |

---

## 1. 目标与范围

### 1.1 目标

1. 新建 `engine/backend_config.h`——配置头，根据 CMake 编译定义选择 IoBackend / MetaIndex 的具体实现类型（using 别名）。
2. 改造 `DeviceContext`：`int fd` → `IoBackendImpl io`；`MetaIndex` → `MetaIndexImpl`。
3. 改造 `Engine::Open / Close / Put / Get`：从直接调 `::open` / `WriteBlock` / `ReadBlock` / `::close` + 旧 MetaIndex，切换到通过抽象层方法调用。
4. CMake 分派生效：`engine/CMakeLists.txt` 根据 `CABE_IO_BACKEND` / `CABE_META_INDEX` 传递编译定义，`backend_config.h` 据此选择具体类型。
5. 删除 P1 遗留的 `engine/io.h` / `engine/io.cpp` / `engine/meta_index.h` / `engine/meta_index.cpp`。
6. 删除对应的旧测试 `test/engine/io_test.cpp` / `test/engine/meta_index_test.cpp`——已被 P3M1 / P3M2 测试覆盖。

### 1.2 交付范围

1. **`engine/backend_config.h`**（新建）：配置头。
2. **`engine/device_context.h`**（修改）：持有抽象层实例。
3. **`engine/engine.cpp`**（修改）：通过抽象层调用。
4. **`engine/CMakeLists.txt`**（修改）：移除旧源文件、链接 `cabe_io` / `cabe_index`、传递编译定义。
5. **`CMakeLists.txt`**（修改）：移除"分派推迟到 P3"的占位提示。
6. **删除**：`engine/io.h`、`engine/io.cpp`、`engine/meta_index.h`、`engine/meta_index.cpp`。
7. **删除**：`test/engine/io_test.cpp`、`test/engine/meta_index_test.cpp`。
8. **`test/CMakeLists.txt`**（修改）：移除 `test_io` 和 `test_meta_index` 目标。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| io_uring 后端接入 | **P4** | P3 只做同步后端 |
| B+ 树索引接入 | **P9** | P3 只做 hashmap |
| BufferHandle / 零拷贝 | **P8** | 继续用裸 `byte*` |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 | 理由 |
|---|---|---|---|
| **P3M3-D1** | 后端切换方式 | 编译期——CMake 参数 + 配置头 using 别名（方案 C） | 生产环境后端固定，运行时切换无实际收益；编译期零开销（ROADMAP D20/D23） |
| **P3M3-D2** | Engine 是否变模板类 | 否——Engine 保持普通类，内部通过 using 别名直接使用具体类型 | 公开 API 不变；无需注入测试替身（正常路径用 loop 设备测试即可；异常路径暂不处理——初期快速攒可用工程） |
| **P3M3-D3** | 旧 I/O / 索引代码处理 | 删除 `engine/io.*` / `engine/meta_index.*` + 对应测试 | P3M1 / P3M2 已提供等价功能和更完善的测试 |

---

## 3. 配置头设计

```cpp
// engine/backend_config.h
#ifndef CABE_BACKEND_CONFIG_H
#define CABE_BACKEND_CONFIG_H

// ---- IoBackend 分派 ----
#if defined(CABE_USE_IO_SYNC)
#include "io/sync/sync_io_backend.h"
namespace cabe { using IoBackendImpl = SyncIoBackend; }
#else
#error "未选择 IoBackend：请设置 -DCABE_IO_BACKEND=sync"
#endif

// ---- MetaIndex 分派 ----
#if defined(CABE_USE_META_HASHMAP)
#include "index/hash/hash_meta_index.h"
namespace cabe { using MetaIndexImpl = HashMetaIndex; }
#else
#error "未选择 MetaIndex：请设置 -DCABE_META_INDEX=hashmap"
#endif

// 编译期验证别名满足对应接口约束
#include "io/io_backend.h"
#include "index/meta_index.h"
static_assert(cabe::IoBackend<cabe::IoBackendImpl>);
static_assert(cabe::MetaIndexBackend<cabe::MetaIndexImpl>);

#endif // CABE_BACKEND_CONFIG_H
```

**设计要点**：
- `#if` / `#else #error` 结构——未选择后端时给出清晰的编译错误。
- 末尾 `static_assert` 确保选中的类型满足对应接口约束——编译期安全网。
- P4 加 io_uring 时只需插入 `#elif defined(CABE_USE_IO_URING)` 分支。
- P9 加 B+ 树时只需插入 `#elif defined(CABE_USE_META_BPLUSTREE)` 分支。

---

## 4. DeviceContext 改造

### 4.1 改造后

```cpp
// engine/device_context.h
#ifndef CABE_DEVICE_CONTEXT_H
#define CABE_DEVICE_CONTEXT_H

#include "engine/backend_config.h"
#include "engine/buffer_pool.h"
#include "engine/free_list.h"

namespace cabe {

    struct DeviceContext {
        IoBackendImpl io;
        BufferPool pool{0};
        FreeList free_list;
        MetaIndexImpl meta_index;
    };

} // namespace cabe

#endif // CABE_DEVICE_CONTEXT_H
```

### 4.2 变更对照

| 字段 | 改造前 | 改造后 |
|---|---|---|
| I/O | `int fd = -1` | `IoBackendImpl io` |
| 索引 | `MetaIndex meta_index`（`engine/meta_index.h`） | `MetaIndexImpl meta_index`（`index/hash/hash_meta_index.h`） |
| 缓冲池 | `BufferPool pool{0}` | 不变 |
| 空闲列表 | `FreeList free_list` | 不变 |

### 4.3 影响

- `DeviceContext` 不再包含裸文件描述符——设备生命周期完全由 `IoBackendImpl` 管理。
- `#include "engine/meta_index.h"` 移除——通过 `backend_config.h` 引入 `HashMetaIndex`。
- `SyncIoBackend` 支持 move 语义——`std::vector<DeviceContext>` 的 `push_back` 正常工作。

---

## 5. Engine 改造

### 5.1 Engine::Open

```cpp
Status Engine::Open(const Options& opts) {
    if (opened_) return Status::Error(err::kEngineAlreadyOpen);
    if (opts.devices.empty()) return Status::Error(err::kEngineInvalidOpts);
    if (opts.devices.size() > 1) return Status::Error(err::kEngineInvalidOpts);

    for (const auto& cfg : opts.devices) {
        DeviceContext dc;

        int32_t rc = dc.io.Open(cfg.path);
        if (rc != err::kSuccess) {
            for (auto& d : devices_) d.io.Close();
            devices_.clear();
            return Status::Error(rc);
        }

        dc.pool = BufferPool(kDefaultPoolBlocks);
        dc.free_list.Init(0, dc.io.BlockCount());
        devices_.push_back(std::move(dc));
    }

    opened_ = true;
    CABE_LOG_INFO("Engine::Open 成功: %zu 个设备", opts.devices.size());
    return Status::Ok();
}
```

**变更要点**：
- `::open(path, O_RDWR | O_DIRECT)` + `::ioctl(BLKGETSIZE64)` → `dc.io.Open(cfg.path)`（SyncIoBackend::Open 内部完成两步）。
- `block_count = dev_bytes / kValueSize` → `dc.io.BlockCount()`（SyncIoBackend::Open 内部已计算）。
- `dc.fd = fd` → 不需要——`dc.io` 已持有打开的设备。
- 错误清理：`::close(fd)` → `d.io.Close()`。
- 移除设备打开 / 大小查询相关的日志调用——SyncIoBackend::Open 内部已有日志。

### 5.2 Engine::Close

```cpp
Status Engine::Close() {
    if (!opened_) return Status::Error(err::kEngineNotOpen);

    for (auto& dc : devices_) {
        dc.io.Close();
    }
    devices_.clear();
    opened_ = false;
    CABE_LOG_INFO("Engine::Close 完成");
    return Status::Ok();
}
```

**变更**：`::close(dc.fd)` + `dc.fd = -1` → `dc.io.Close()`。

### 5.3 Engine::Put

```cpp
// 写设备（取代 WriteBlock(dc.fd, ...)）
rc = dc.io.Write(block_id.block_idx(), buf);
```

单行替换：`WriteBlock(dc.fd, block_id.block_idx(), buf)` → `dc.io.Write(block_id.block_idx(), buf)`。
其余逻辑（分配块、填 buffer、更新索引）不变。

### 5.4 Engine::Get

```cpp
// 读设备（取代 ReadBlock(dc.fd, ...)）
rc = dc.io.Read(meta.block.block_idx(), buf);
```

单行替换：`ReadBlock(dc.fd, meta.block.block_idx(), buf)` → `dc.io.Read(meta.block.block_idx(), buf)`。
CRC 校验逻辑不变。

### 5.5 Engine::Delete

不变——Delete 不涉及 I/O（只操作 FreeList + MetaIndex）。`dc.meta_index.Lookup` / `Delete` 方法签名与旧 MetaIndex 完全相同。

### 5.6 移除的 include

```diff
- #include "engine/io.h"
- #include <fcntl.h>
- #include <linux/fs.h>
- #include <sys/ioctl.h>
- #include <unistd.h>
```

`backend_config.h` 通过 `device_context.h` 间接引入，无需新增显式 include。
系统头文件（`fcntl.h` / `linux/fs.h` / `sys/ioctl.h` / `unistd.h`）由 SyncIoBackend 内部包含，Engine 不再直接使用。

---

## 6. CMake 分派

### 6.1 engine/CMakeLists.txt（改造后）

```cmake
# P3-M3：Engine 切换到抽象层 + CMake 后端分派。
# 设计依据：doc/P3/P3M3_engine_switch_design.md
add_library(cabe_engine STATIC
    engine.cpp
    buffer_pool.cpp
    free_list.cpp
)
target_link_libraries(cabe_engine PUBLIC cabe_util cabe_io cabe_index)
add_library(cabe::engine ALIAS cabe_engine)

# ---- IoBackend 编译期分派 ----
if(CABE_IO_BACKEND STREQUAL "sync")
    target_compile_definitions(cabe_engine PUBLIC CABE_USE_IO_SYNC=1)
else()
    message(FATAL_ERROR "CABE_IO_BACKEND='${CABE_IO_BACKEND}' 尚未实现")
endif()

# ---- MetaIndex 编译期分派 ----
if(CABE_META_INDEX STREQUAL "hashmap")
    target_compile_definitions(cabe_engine PUBLIC CABE_USE_META_HASHMAP=1)
else()
    message(FATAL_ERROR "CABE_META_INDEX='${CABE_META_INDEX}' 尚未实现")
endif()
```

**变更对照**：

| 项 | 改造前 | 改造后 |
|---|---|---|
| 源文件 | `engine.cpp buffer_pool.cpp io.cpp free_list.cpp meta_index.cpp` | `engine.cpp buffer_pool.cpp free_list.cpp`（移除 `io.cpp` / `meta_index.cpp`） |
| 链接 | `cabe_util` | `cabe_util cabe_io cabe_index`（新增 `cabe_io` / `cabe_index`） |
| 编译定义 | 无 | `CABE_USE_IO_SYNC` / `CABE_USE_META_HASHMAP`（PUBLIC——因 `backend_config.h` 被 `device_context.h` 引入，链接 `cabe_engine` 的目标编译时也需要此定义） |

### 6.2 根 CMakeLists.txt 修改

移除 P3 前的占位提示：

```diff
- if(NOT CABE_IO_BACKEND STREQUAL "sync")
-     message(STATUS "CABE_IO_BACKEND='${CABE_IO_BACKEND}' recorded; real dispatch lands in P3.")
- endif()
- if(NOT CABE_META_INDEX STREQUAL "hashmap")
-     message(STATUS "CABE_META_INDEX='${CABE_META_INDEX}' recorded; real dispatch lands in P3/P9.")
- endif()
```

根 CMakeLists.txt 的 `cabe_validate_choice` 校验保留不变——它验证输入值合法性；`engine/CMakeLists.txt` 的 `FATAL_ERROR` 验证实现可用性。两层校验互补。

### 6.3 test/CMakeLists.txt 修改

移除以下两个测试目标：

```diff
- add_executable(test_io engine/io_test.cpp)
- target_link_libraries(test_io PRIVATE cabe::engine GTest::gtest_main)
- gtest_discover_tests(test_io DISCOVERY_TIMEOUT 60)

- add_executable(test_meta_index engine/meta_index_test.cpp)
- target_link_libraries(test_meta_index PRIVATE cabe::engine GTest::gtest_main)
- gtest_discover_tests(test_meta_index DISCOVERY_TIMEOUT 60)
```

### 6.4 改造后依赖链

```
cabe_engine (STATIC)
  ├─► cabe_util (STATIC)     ← 原有（CRC32、时间戳、哈希路由）
  ├─► cabe_io (STATIC)       ← P3M3 新增
  └─► cabe_index (STATIC)    ← P3M3 新增
```

---

## 7. 旧代码清理

| 文件 | 处理 | 原因 |
|---|---|---|
| `engine/io.h` | **删除** | `WriteBlock` / `ReadBlock` 被 `SyncIoBackend::Write` / `Read` 取代 |
| `engine/io.cpp` | **删除** | 同上 |
| `engine/meta_index.h` | **删除** | `MetaIndex` 类被 `HashMetaIndex` 取代 |
| `engine/meta_index.cpp` | **删除** | 同上 |
| `test/engine/io_test.cpp` | **删除** | `WriteBlock` / `ReadBlock` 测试被 `test_sync_io_backend` 覆盖 |
| `test/engine/meta_index_test.cpp` | **删除** | 旧 `MetaIndex` 测试被 `test_meta_index_contract` 覆盖 |

### 7.1 测试覆盖对照

| 旧测试 | 旧用例数 | 替代测试 | 替代用例数 | 说明 |
|---|---|---|---|---|
| `test_io` | 2 | `test_sync_io_backend` | 10 | 覆盖更完整（含 Open/Close/Move/BadPath） |
| `test_meta_index` | 7 | `test_meta_index_contract` | 10 | 覆盖更完整（含 ForEach / 快照空壳验证） |

旧 `test_meta_index` 的 `MoveConstruct` 用例未在契约测试中——move 语义是实现细节，不属于接口契约。`HashMetaIndex` 的 move 由 `unordered_map` 默认 move 保证，通过 `DeviceContext` 在 `vector::push_back` 中隐式验证。

---

## 8. 测试策略

### 8.1 现有测试预期

Engine 的所有测试（`test_engine`）不修改——公开 API 不变，只是内部实现路径从裸 fd + WriteBlock 切换到 IoBackendImpl + MetaIndexImpl。

| 测试目标 | 用例数 | 预期 |
|---|---|---|
| `test_engine`（需设备） | 14 | 全绿——功能等价 |
| `test_engine`（无设备） | 4 | 全绿 |
| `test_sync_io_backend` | 10 | 不受影响 |
| `test_meta_index_contract` | 10 | 不受影响 |
| 其他（util / common / buffer_pool / free_list / status） | ~40 | 不受影响 |

### 8.2 验证方式

1. 全量测试：`run-tests.sh` 四档（release / asan / tsan / ubsan）全绿。
2. 覆盖率 ≥ 80%。
3. 确认 CMake 分派生效：设置 `CABE_IO_BACKEND=io_uring` → CMake 配置阶段报 `FATAL_ERROR`。

---

## 9. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 编译定义传播 | `CABE_USE_IO_SYNC` 等定义通过 PUBLIC 传播到所有链接 `cabe_engine` 的目标 | cabe 是独立项目，不作为第三方库发布——可接受 |
| 同一二进制无法链接两个后端 | 编译期只能选一个 IoBackend / MetaIndex | 这是 ROADMAP D23 锁定的设计决策 |
| 删除旧代码后无法回退 | 旧 I/O / 索引代码永久删除 | git 历史可追溯；P3M1/P3M2 提供了等价且更完善的实现 |

---

## 10. 退出条件

1. **配置头就位**：`engine/backend_config.h` 含 `IoBackendImpl` / `MetaIndexImpl` 别名 + `static_assert` 验证。
2. **DeviceContext 改造完成**：持有 `IoBackendImpl io` + `MetaIndexImpl meta_index`。
3. **Engine 通过抽象层调用**：不再出现 `::open` / `::close` / `WriteBlock` / `ReadBlock` 直接调用。
4. **CMake 分派生效**：`-DCABE_IO_BACKEND=sync -DCABE_META_INDEX=hashmap` 编译通过；设置其他值报错。
5. **旧代码删除**：`engine/io.*` / `engine/meta_index.*` / `test/engine/io_test.cpp` / `test/engine/meta_index_test.cpp` 已删除。
6. **测试不退步**：Engine 的 18 个用例全绿。
7. **四档全绿**：release / asan / tsan / ubsan。
8. **覆盖率** ≥ 80%。

---

## 11. 对下游里程碑的接口承诺

| 下游 | 接入点 |
|---|---|
| **P3M4** | 收敛检查——确认 P3 四个里程碑完成，ROADMAP / README 状态同步 |
| **P4** | `engine/CMakeLists.txt` 加 `elseif(CABE_IO_BACKEND STREQUAL "io_uring")` + `engine/backend_config.h` 加 `#elif CABE_USE_IO_URING` |
| **P9** | `engine/CMakeLists.txt` 加 `elseif(CABE_META_INDEX STREQUAL "bplustree")` + `engine/backend_config.h` 加 `#elif CABE_USE_META_BPLUSTREE` |

---

**全文完。**
