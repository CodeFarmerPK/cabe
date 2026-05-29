# Cabe P1-M1 设计：引擎骨架 + 公开类型

> 本里程碑建立 `engine/` 目录与 CMake 子目录，定义三个公开类型（`Options`、`Status`、`Engine`）
> 以及内部结构 `DeviceContext`，实装 `Engine::Open` / `Close` 状态机；Put / Get / Delete 仅定义
> 签名（空壳，返回"未实装"错误码），完整路径留给 P1M4。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P1 / M1 |
| 状态 | **✅ 已锁定（P1M5 收敛）**
| 上游依赖 | P0 全部完成（`common/structs.h` / `error_code.h` / `logger.h` / `util/*`） |
| 下游依赖本里程碑 | P1M2（BufferPool + 朴素 I/O）、P1M3（FreeList + MetaIndex）、P1M4（端到端路径） |
| 关联约束 | ROADMAP P1 范围；ROADMAP P2 段（Options / Status / Engine 在 P2 冻结） |
| 退出判定 | 见 §11（六条） |

---

## 1. 目标与范围

### 1.1 目标

1. 建立 `engine/` 目录与 CMake 目标 `cabe_engine`（STATIC），接入已有的 `cabe_util` / `cabe_common` 依赖链。
2. 定义三个公开类型：`cabe::Options`（引擎配置）、`cabe::Status`（操作结果）、`cabe::Engine`（引擎本体），各自独立头文件。
3. 定义内部结构 `cabe::DeviceContext`（P1M1 只含 `int fd`，后续 milestone 逐步补入）。
4. 实装 `Engine::Open(opts)` / `Close()` 状态机（Closed ↔ Opened）。
5. 定义 `Put` / `Get` / `Delete` 方法签名（空壳，返回"未实装"错误码）。
6. 扩展 `common/error_code.h` 的 engine 段，新增 P1M1 所需的错误码。
7. 编写 Open / Close 状态机 + Status 类型的单元测试。

### 1.2 交付范围（本里程碑产出）

1. **`engine/options.h`**：`Options` + `DeviceConfig` 定义。
2. **`engine/status.h`**：`Status` 薄包装 struct。
3. **`engine/engine.h` + `engine/engine.cpp`**：`Engine` 类声明与实现。
4. **`engine/device_context.h`**：`DeviceContext` struct（内部头文件，不对用户暴露）。
5. **`engine/CMakeLists.txt`**：CMake 子目录。
6. **根 `CMakeLists.txt` 修改**：加 `add_subdirectory(engine)`。
7. **`common/error_code.h` 修改**：engine 段新增 4 个错误码。
8. **`test/engine/` 新增测试**：`engine_test.cpp` + `status_test.cpp`。
9. **`test/CMakeLists.txt` 修改**：注册 P1M1 新增测试。

### 1.3 推迟范围（明确推迟，标注落点）

| 推迟项 | 落点 | 原因 |
|---|---|---|
| BufferPool 实装 | **P1M2** | 数据通路基础设施里程碑 |
| 朴素 I/O（pread / pwrite + O_DIRECT） | **P1M2** | 同上 |
| FreeList / MetaIndex | **P1M3** | 索引与空间管理里程碑 |
| Put / Get / Delete 完整路径实装 | **P1M4** | 端到端打通里程碑 |
| Options / Status / Engine API 冻结 | **P2** | P1 初版、P2 冻结 |
| Engine 并发安全 | **P7** | P1–P6 单线程访问 |

---

## 2. 现状盘点

- **`engine/` 目录不存在**——从空开始。
- **P0 已有的接入点**：
  - `common/structs.h`：`BlockId` / `ValueMeta` / `DataView` / `DataBuffer` / `kValueSize` / `ValueState`
  - `common/error_code.h`：`kSuccess` + 六段错误码（`kEngineBase = -104000` 已预留、未分配具体码）
  - `common/logger.h`：`CABE_LOG_*` 宏
  - `util/hash.h`：`Hash()` / `RouteToDevice()`
  - `util/crc32.h`：`CRC32()`
- **ROADMAP P2 已给的 Options 骨架**：`struct Options { std::vector<DeviceContext> devices; }`——但 `DeviceContext` 是 Engine 内部构建的运行时对象，不应直接暴露到 Options 配置里。P1 初版改为 `std::vector<DeviceConfig> devices`（DeviceConfig 含路径字串）。
- **ROADMAP P1 "无持久化"语义**：没有 WAL / crash recovery / snapshot。`Engine::Open` 永远从零开始（初始化空索引 + 全量 FreeList），不恢复已有数据。P5 才引入持久化能力。

---

## 3. 关键决策（owner 已拍板）

| 编号 | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| **P1M1-D1** | 三者独立头文件暴露（`engine/options.h` / `engine/status.h` / `engine/engine.h`） | 仅暴露 Engine（Options/Status 嵌套或用裸 int） | ROADMAP P2 已把 Options / Status 作为独立类型使用；P1 按此形态初版 | 锁定 |
| **P1M1-D2** | Engine 禁止拷贝 + 禁止移动（`= delete`） | 允许 move（自定义 move 语义复杂；P7 多线程共享不适 move） | 最安全最简；需要堆分配用 `std::unique_ptr<Engine>` | 锁定 |
| **P1M1-D3** | Status = 薄包装 struct（`int code` + `ok()` + `explicit operator bool()` + `Ok()` / `Error(c)` 工厂方法），4 字节，trivially copyable | 裸 int（类型不安全）/ 带 message（堆分配）/ std::expected（C++23） | cabe 六段 × 1000 错误码精细度够，不需 message；日志由 `CABE_LOG_*` 覆盖；不用异常 | 锁定 |
| **P1M1-D4** | DeviceContext = struct 公开成员（Engine 内部直接访问） | class 封装（过早抽象，违反设计原则 #2） | P1 不做抽象层；P3 引入时再重构 | 锁定 |
| **P1M1-D5** | Engine::Open = 空构造 + `Open(opts)` / `Close()` 成员函数；Closed ↔ Opened 状态机 | 工厂函数（不支持幂等）/ 构造函数做 Open（不用异常则半初始化） | 自然支持 P2 幂等语义；析构自动 Close + 日志警告 | 锁定 |

---

## 4. 目录与 CMake 结构

### 4.1 新增目录

```
engine/
├── CMakeLists.txt      # CMake 子目录
├── engine.h            # Engine class 声明
├── engine.cpp          # Engine 实现
├── options.h           # Options + DeviceConfig
├── status.h            # Status struct
└── device_context.h    # DeviceContext（内部头，不对用户暴露）

test/engine/
├── engine_test.cpp     # Open / Close 状态机 + 析构自动 Close
└── status_test.cpp     # Status 工厂方法 + ok() + operator bool() + 比较
```

### 4.2 CMake 目标

**`engine/CMakeLists.txt`**：
```cmake
# P1-M1：引擎骨架。
# 设计依据：doc/P1/P1M1_engine_skeleton_design.md
add_library(cabe_engine STATIC engine.cpp)
target_link_libraries(cabe_engine PUBLIC cabe_util)    # 传递含 cabe_common → cabe_flags
target_include_directories(cabe_engine PUBLIC ${PROJECT_SOURCE_DIR})
add_library(cabe::engine ALIAS cabe_engine)
```

**根 `CMakeLists.txt` 修改**：在 `add_subdirectory(util)` 之后加一行：
```cmake
add_subdirectory(engine)
```

**`test/CMakeLists.txt` 修改**：
```cmake
cabe_add_test(test_engine       engine/engine_test.cpp)
cabe_add_test(test_status       engine/status_test.cpp)
```

注：测试目标链接 `cabe::engine`（含 `cabe::util` + `cabe::common` 传递）。`cabe_add_test` 当前链接
`cabe::util + cabe::common`——需要改为同时链接 `cabe::engine`，或把 `cabe_add_test` 的默认链接列表扩展。

推荐做法：P1M1 测试目标**单独写**（不走 `cabe_add_test`），显式链接 `cabe::engine`：
```cmake
add_executable(test_engine engine/engine_test.cpp)
target_link_libraries(test_engine PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_engine DISCOVERY_TIMEOUT 60)

add_executable(test_status engine/status_test.cpp)
target_link_libraries(test_status PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_status DISCOVERY_TIMEOUT 60)
```

或者扩展 `cabe_add_test` 加可选 `LIBS` 参数——但改公共函数影响 P0 测试，P1M1 不宜动公共约定。
推迟到 P1M5 收敛时统一决定是否改 `cabe_add_test`。

### 4.3 依赖链

```
cabe_flags (INTERFACE)
  └─► cabe_common (INTERFACE)
        └─► cabe_util (STATIC)
              └─► cabe_engine (STATIC)    ← P1M1 新增
                    └─► test_engine / test_status (TEST)
```

---

## 5. Options 设计

### 5.1 DeviceConfig

ROADMAP P2 段给的 Options 含 `std::vector<DeviceContext> devices`——但 `DeviceContext` 是 Engine
内部运行时状态（含 fd / BufferPool / 索引），不应暴露到用户配置。P1M1 引入 `DeviceConfig` 作为
用户侧设备配置：

```cpp
// engine/options.h
#ifndef CABE_OPTIONS_H
#define CABE_OPTIONS_H

#include <string>
#include <vector>

namespace cabe {

    struct DeviceConfig {
        // P5 起：一个设备组 = 数据 + WAL + 快照三块设备
        std::string data_path;       // 数据设备（如 "/dev/nvme0n1"）
        std::string wal_path;        // WAL 设备
        std::string snapshot_path;   // 快照设备
    };

    struct Options {
        std::vector<DeviceConfig> devices;  // N 在 Open 时固定（D8）；P1 内 size() == 1
    };

} // namespace cabe

#endif // CABE_OPTIONS_H
```

**设计要点**：
- `DeviceConfig` 目前只含 `path`；P2 冻结时可加 `size_t capacity_blocks` / `size_t block_size` 等（预留但不提前加——设计原则 #2）。
- `Options` 目前只含 `devices`；P2 冻结时可加 `reserved` 字段（ROADMAP P2 段提及）。
- P1 内要求 `opts.devices.size() == 1`；>1 时 `Open` 返回 `kEngineInvalidOpts`。

---

## 6. Status 设计

```cpp
// engine/status.h
#ifndef CABE_STATUS_H
#define CABE_STATUS_H

#include "common/error_code.h"

#include <compare>

namespace cabe {

    struct Status {
        int code = err::kSuccess;

        constexpr bool ok() const noexcept { return code == err::kSuccess; }
        constexpr explicit operator bool() const noexcept { return ok(); }

        static constexpr Status Ok() noexcept { return Status{err::kSuccess}; }
        static constexpr Status Error(int c) noexcept { return Status{c}; }

        constexpr auto operator<=>(const Status&) const noexcept = default;
    };

} // namespace cabe

#endif // CABE_STATUS_H
```

**设计要点**：
- 4 字节，`trivially_copyable`，`standard_layout`——加 `static_assert` 守护。
- `operator bool` 标 `explicit`——避免 `Status` 隐式参与算术。
- `Ok()` / `Error(c)` 工厂方法——调用方写 `return Status::Ok();` 比 `return Status{0};` 更可读。
- `operator<=>` defaulted——方便测试 `EXPECT_EQ(status, Status::Ok())`。
- **不含 message**——cabe 六段 × 1000 错误码足以定位原因；上下文由 `CABE_LOG_*` 覆盖。
- P2 冻结时可按需加字段（如 `DeviceId failing_device`、`uint8_t reserved[3]` 对齐到 8 字节）。

---

## 7. DeviceContext 设计

```cpp
// engine/device_context.h（内部头文件，不对用户暴露）
#ifndef CABE_DEVICE_CONTEXT_H
#define CABE_DEVICE_CONTEXT_H

namespace cabe {

    struct DeviceContext {
        int fd = -1;  // 打开的文件 / 设备描述符；-1 = 未打开
        // P1M2 补入：BufferPool pool;
        // P1M3 补入：std::vector<BlockId> free_list;
        // P1M3 补入：std::unordered_map<std::string, ValueMeta> index;
    };

} // namespace cabe

#endif // CABE_DEVICE_CONTEXT_H
```

**设计要点**：
- struct 公开成员（P1M1-D4 锁定）；Engine 内部直接访问 `dc.fd`。
- P1M1 只含 `fd`——后续 milestone 逐步补入（每个 milestone 只加自己那层的字段）。
- 不暴露给用户（不在公开 API 承诺内）。

---

## 8. Engine 设计

### 8.1 类声明

```cpp
// engine/engine.h
#ifndef CABE_ENGINE_H
#define CABE_ENGINE_H

#include "engine/options.h"
#include "engine/status.h"
#include "common/structs.h"

#include <string_view>
#include <vector>

namespace cabe {

    // 前置声明（内部类型，不暴露给用户头文件）
    struct DeviceContext;

    class Engine {
    public:
        Engine() noexcept = default;
        ~Engine();

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&) = delete;
        Engine& operator=(Engine&&) = delete;

        Status Open(const Options& opts);
        Status Close();

        Status Put(std::string_view key, DataView value);
        Status Get(std::string_view key, DataBuffer value);
        Status Delete(std::string_view key);

        bool is_open() const noexcept;

    private:
        bool opened_ = false;
        std::vector<DeviceContext> devices_;
    };

} // namespace cabe

#endif // CABE_ENGINE_H
```

### 8.2 Open / Close 状态机

```
           Open(opts)           Close()
  Closed ──────────► Opened ──────────► Closed
    │                  │                  │
    │  Open(opts)      │  Open(opts)      │  Close()
    │  → 正常打开      │  → kAlreadyOpen  │  → kNotOpen
```

**`Open(opts)` 行为**：
1. 已 Opened → 返回 `Status::Error(err::kEngineAlreadyOpen)`
2. 校验 `opts.devices.empty()` → 返回 `Status::Error(err::kEngineInvalidOpts)`
3. 校验 `opts.devices.size() > 1`（P1 内仅支持单设备）→ 返回 `Status::Error(err::kEngineInvalidOpts)`
4. 打开设备/文件（`open(path, O_RDWR | O_CREAT, 0600)`——P1M1 骨架不加 O_DIRECT，P1M2 补）
5. 构建 `DeviceContext`（`fd` 设为打开的描述符）
6. `opened_ = true` → 返回 `Status::Ok()`

**`Close()` 行为**：
1. 未 Opened → 返回 `Status::Error(err::kEngineNotOpen)`
2. 关闭所有 DeviceContext 的 fd（`::close(dc.fd)`）
3. 清空 `devices_` → `opened_ = false` → 返回 `Status::Ok()`

**析构**：
```cpp
Engine::~Engine() {
    if (opened_) {
        CABE_LOG_WARN("Engine 析构时仍处于 Opened 状态，自动 Close");
        Close();  // 忽略返回值——析构不该抛
    }
}
```

### 8.3 Put / Get / Delete 空壳（P1M1 签名就位）

```cpp
Status Engine::Put(std::string_view key, DataView value) {
    if (!opened_) return Status::Error(err::kEngineNotOpen);
    if (key.empty()) return Status::Error(err::kMemEmptyKey);
    if (value.size() != kValueSize) return Status::Error(err::kEngineInvalidValue);
    // P1M4 实装完整路径
    return Status::Error(err::kEngineNotImplemented);
}

Status Engine::Get(std::string_view key, DataBuffer value) {
    if (!opened_) return Status::Error(err::kEngineNotOpen);
    if (key.empty()) return Status::Error(err::kMemEmptyKey);
    if (value.size() != kValueSize) return Status::Error(err::kEngineInvalidValue);
    // P1M4 实装完整路径
    return Status::Error(err::kEngineNotImplemented);
}

Status Engine::Delete(std::string_view key) {
    if (!opened_) return Status::Error(err::kEngineNotOpen);
    if (key.empty()) return Status::Error(err::kMemEmptyKey);
    // P1M4 实装完整路径
    return Status::Error(err::kEngineNotImplemented);
}
```

**设计要点**：
- 前置校验（`is_open` / 空 key / value 大小）在 P1M1 就实装——P1M4 只需补路径逻辑，不重做校验。
- 空 key 复用 P0 已有的 `err::kMemEmptyKey`（段位不限模块——memory 段本来就是"调用方编程错误"）。
- value 大小校验复用同一错误码——或新建 `kEngineInvalidValue`（见 §9）。

---

## 9. 错误码扩展

在 `common/error_code.h` 的 engine 段（`kEngineBase = -104000`）新增以下码：

```cpp
// ---- engine 段（P1M1 新增）----
inline constexpr int kEngineAlreadyOpen    = InSeg(kEngineBase, 0);  // -104000
inline constexpr int kEngineNotOpen        = InSeg(kEngineBase, 1);  // -104001
inline constexpr int kEngineInvalidOpts    = InSeg(kEngineBase, 2);  // -104002
inline constexpr int kEngineInvalidValue   = InSeg(kEngineBase, 3);  // -104003
inline constexpr int kEngineNotImplemented = InSeg(kEngineBase, 4);  // -104004

// 越段守护
static_assert(kEngineNotImplemented > kEngineBase - kSegmentSize);
```

| 码 | 值 | 含义 | 触发场景 |
|---|---|---|---|
| `kEngineAlreadyOpen` | -104000 | 重复 Open | `Open` 在已 Opened 状态被调用 |
| `kEngineNotOpen` | -104001 | 未 Open 就操作 | `Close` / `Put` / `Get` / `Delete` 在 Closed 状态被调用 |
| `kEngineInvalidOpts` | -104002 | Options 不合法 | `opts.devices.empty()` 或 P1 内 `size() > 1` |
| `kEngineInvalidValue` | -104003 | value 大小不对 | `value.size() != kValueSize` |
| `kEngineNotImplemented` | -104004 | 功能未实装 | P1M1 空壳返回；P1M4 实装后不再使用 |

---

## 10. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| Options 形态 P1 vs P2 不一致 | ROADMAP P2 写 `vector<DeviceContext>` 但 P1 用 `vector<DeviceConfig>` | DeviceConfig 是用户侧配置、DeviceContext 是内部运行时——语义不同。P2 冻结时评估是否对齐命名 |
| P1M1 Open 不加 O_DIRECT | P1M2 才加 O_DIRECT——P1M1 骨架用普通 `open()` | P1M2 修改 Open 实现加 O_DIRECT 标志，接口不变 |
| `kEngineNotImplemented` 临时码 | P1M4 实装后该码不再使用 | P1M5 收敛时评估是否保留（作为"未来模块"的占位码）还是删除 |
| DeviceContext 前置声明 vs include | `engine.h` 对 DeviceContext 做前置声明 → 用户不依赖内部头 | `engine.cpp` `#include "engine/device_context.h"` 拿到完整定义；`engine.h` 里 `vector<DeviceContext>` 需要完整类型——前置声明不够；需改为 `vector<unique_ptr<DeviceContext>>` 或直接 include 内部头 |
| `engine.h` include 内部头 | 若 `engine.h` 直接 `#include "engine/device_context.h"` → 用户能 include 内部头 | P1 阶段不做公开/私有头分离（P2 冻结时再拆）；或用 `unique_ptr<DeviceContext>` + 前置声明（pimpl 轻量版） |

**`vector<DeviceContext>` 的前置声明问题**：

`std::vector<T>` 要求 `T` 在声明 vector 成员时是完整类型。如果 `engine.h` 只做前置声明 `struct DeviceContext;` 而把 `std::vector<DeviceContext> devices_;` 放到 private——**编译会失败**。

两种解法：
- **方案 α**：`engine.h` 直接 `#include "engine/device_context.h"`——简单但内部头对用户可见
- **方案 β**：用 `std::unique_ptr<DeviceContext>` + 前置声明（pimpl 轻量版）：
  ```cpp
  struct DeviceContext; // 前置声明
  std::vector<std::unique_ptr<DeviceContext>> devices_;
  ```
  开销：多一层指针间接；但隔离了内部类型。

**推荐方案 α**——P1 不做公开/私有头分离（P2 冻结时再拆），DeviceContext 虽对用户可见但不承诺稳定。在 `device_context.h` 头部加注释标明"内部类型、不在公开 API 承诺内"。

---

## 11. 退出条件与验证步骤

### 11.1 退出条件

1. **目录与构建**：`engine/` 目录就位 + `cabe_engine` CMake 目标双工具链编译通过。
2. **公开类型完整**：`Options` / `Status` / `Engine` 三者各自独立头文件 + `static_assert` 守护
   （`Status` trivially copyable / 4 字节）。
3. **状态机行为**：`Open` / `Close` 状态机有单元测试覆盖（正常流程 + 重复 Open + 重复 Close +
   析构自动 Close）。
4. **空壳签名**：`Put` / `Get` / `Delete` 签名就位 + 前置校验有测试覆盖（未 Open / 空 key /
   value 大小不对 → 返回对应错误码）。
5. **四档全绿**：`run-tests.sh` 分别以 `--asan` / `--tsan` / `--ubsan` / `--release` 跑通（含 P1M1 新增用例）。
6. **覆盖率**：`run-coverage.sh --strict` 行覆盖率 ≥ 80%。

### 11.2 验证步骤（建议顺序）

1. `cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCABE_BUILD_TESTS=ON && cmake --build build`
   —— 确认 `cabe_engine` 编译通过。
2. `ctest --test-dir build` —— 确认新增的 `test_engine` / `test_status` 用例全绿。
3. `./scripts/run-tests.sh --asan && ./scripts/run-tests.sh --tsan && ./scripts/run-tests.sh --ubsan && ./scripts/run-tests.sh --release` —— 四档回归。
4. `./scripts/run-coverage.sh --strict` —— 覆盖率 ≥ 80%。

---

## 12. 对下游里程碑的接口承诺

| 下游 | 本里程碑提供的接入点 |
|---|---|
| **P1M2** | `DeviceContext` struct 可直接补入 `BufferPool` 字段；`Engine::Open` 可直接补入 O_DIRECT 标志 |
| **P1M3** | `DeviceContext` 可补入 `free_list` / `index` 字段；`Engine` 可补入 `RouteKey` 方法 |
| **P1M4** | `Put` / `Get` / `Delete` 空壳体可直接替换为完整路径实现，前置校验已就位 |
| **P2** | `Options` / `Status` / `Engine` 三者独立头文件已就位，P2 只调签名 / 加字段 / 冻结——不改类型归属 |

---

## 13. 与 ROADMAP 一致性核对

| ROADMAP P1 字面 | 本设计实现 | 状态 |
|---|---|---|
| `cabe::Options` / `cabe::Status` 公开类型 | §5 Options + §6 Status | ✅ |
| `cabe::Engine` 公开类骨架 Open / Put / Get / Delete / Close | §8 Engine + 状态机 + 空壳签名 | ✅ |
| 内部按 per-device 形态 `vector<DeviceContext>` P1 内 `size() == 1` | §7 DeviceContext + §8 `devices_` + §5 P1 限制 `size() == 1` | ✅ |
| `struct DeviceContext { IoBackend io; FreeList free; MetaIndex index; }` 雏形 | §7 DeviceContext（P1M1 只含 fd；后续补入）| ✅（渐进式补入） |
| key 路由函数 RouteKey P1 内永远返回 0 | 推迟到 **P1M3**（索引与空间管理） | ✅（按里程碑划分） |

---

**全文完。**
