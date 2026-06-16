# Cabe P0-M3 设计：错误码段位规划 + Logger stderr 实装

> 本里程碑把 `common/error_code.h` 从"单段 `#define`"扩展为**六段错误码空间**（每段 1000 号，
> 带编译期不重叠断言），并把 `common/logger.h` 从**全空操作存根**升级为 **stderr 最简实现**
> （五级、`CABE_LOG_LEVEL` 环境变量控级、`[LEVEL][tid][file:line] message` 格式）。
> **本文为详细设计，暂不生成代码**；其中 C++ 片段均为设计示意。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P0 / M3 |
| 状态 | **✅ 已锁定（P0M7 收敛）** |
| 上游依赖 | M1（构建骨架）、M2（`namespace cabe` 约定，若终审采纳则 M3 跟进） |
| 下游依赖本里程碑 | P1+（`Engine` 用错误码返回 / `Status`）、全阶段（日志宏） |
| 关联约束 | ROADMAP P0「错误码段位规划」「Logger 禁止全空操作」；错误码六段 memory/io/index/wal/engine/wal_recovery |
| 退出判定 | 小 demo 能打出五级日志且受 `CABE_LOG_LEVEL` 过滤；段位不重叠的 `static_assert` 通过；双工具链构建零警告 |

---

## 1. 目标与范围

### 1.1 目标

1. `error_code.h`：六段错误码空间定型，每段 1000 号，保留现有 memory 段取值，段间不重叠由编译期断言保证。
   （P5M4 注：按 P2M1 §4.3 扩展约定在 `-106000` 起新增第七段 **snapshot**——六段定型时预留的"不够用再加段"通道首次启用，原六段未受任何扰动；P5 终态七段占用见 P5M7 收敛稿 §6。）
2. `logger.h`（**纯头宏**，无 `.cpp`）：stderr 最简实现，五级日志、运行期 `CABE_LOG_LEVEL` 控级、统一格式、调用点零改动（沿用 `CABE_LOG_*` 宏名）。

### 1.2 交付范围（本里程碑产出）

1. 重写 `common/error_code.h`：六段基址 + 段容量常量 + memory 段具体码 + 不重叠 `static_assert`。
2. 重写 `common/logger.h`（**纯头**）：`CABE_LOG_*` 宏 + `cabe::log` 内联函数（`Threshold`/`Enabled`/`Name`/`Basename`）+ 级别枚举；宏体单次 `std::fprintf` 输出。
3. **不新增任何 `.cpp`、不改动 `common/CMakeLists.txt`**：logger 是纯头宏实现，`cabe_common` 保持 INTERFACE 头库（见 §6）。

### 1.3 推迟范围（明确推迟，标注落点）

| 推迟项 | 落点 | 原因 |
|---|---|---|
| `Status` 类型（封装错误码 + 消息） | **P1/P2** | M3 只定义码值，`Status` 是公开 API 的一部分 |
| io / index / wal / engine / wal_recovery 段的**具体码** | 各模块起始阶段 | M3 只划段 + 定基址；具体码随模块产生时补 |
| 日志落盘 / 滚动 / 异步队列 / 采样 | 不做（超出范围） | M3 是"最简 stderr"；高级日志非项目目标 |
| Metrics / 慢日志 / 可观测性导出 | **P5（Metrics 接口）/ P12（导出）** | 与日志分属不同子系统 |
| 编译期级别裁剪（`CABE_MIN_LOG_LEVEL` 把低级宏编译掉） | 可选，需要时再加 | M3 用运行期过滤即可满足"env 控级" |

---

## 2. 现状盘点（读码结论）

**`common/error_code.h`（当前）**：全局 `#define`，仅 memory 单段。
```cpp
#define SUCCESS 0
#define MEMORY_NULL_POINTER_EXCEPTION (-100000)
#define MEMORY_EMPTY_KEY              (-100001)
#define MEMORY_EMPTY_VALUE            (-100002)
#define MEMORY_INSERT_FAIL            (-100003)
```

**`common/logger.h`（当前）**：全空操作宏，五级（`DEBUG/INFO/WARN/ERROR/FATAL`），但头部注释只列了 4 级语义（漏 `INFO`）。
```cpp
#define CABE_LOG_DEBUG(fmt, ...) ((void)0)   // … 五个宏全部 ((void)0)
```

**依赖现状**：当前两个 `.cpp`（`crc32` / `cpu_features`）都**不引用** error_code / logger，故 M3 重构**无现存调用点需要适配**，可自由定形。`logger` 头部注释提到"`engine_api.cpp` 等调用点"是未来（P1+）的。

**与 ROADMAP 的差距**：
- error_code 只有 memory 段，缺 io/index/wal/engine/wal_recovery 五段。
- logger 是全空操作 —— ROADMAP P0 明确"**禁止全空操作**"，M3 必须实装 stderr。

---

## 3. 待 owner 终审的决策

### 决策-1：命名空间归属（延续 M2 决策-1）

| 维度 | 内容 |
|---|---|
| 现状 | error_code 用全局 `#define`；logger 用全局宏 |
| 本设计裁决 | **建议**：错误码进 `namespace cabe::err`（`constexpr int`），日志运行期入口进 `namespace cabe::log`；`CABE_LOG_*` 宏名保留（宏无命名空间，加 `CABE_` 前缀防冲突） |
| 依赖 | 与 M2 决策-1（schema 进 `cabe`）是同一个"全工程命名空间约定"，**建议一并终审**：`cabe::`(核心)、`cabe::util`/`cabe::log`/`cabe::err`(子模块) |
| 回退 | 若否决命名空间，error_code 可退回带前缀的全局 `constexpr`，logger 同理 |

### 决策-2：错误码用 `constexpr int`，弃 `#define`

| 维度 | 内容 |
|---|---|
| 备选 | 保留 `#define` / 改 `enum class Errc : int` / 改 `inline constexpr int` |
| 本设计裁决 | **`inline constexpr int`**（放 `cabe::err`）：类型安全、可参与 `static_assert`（段位不重叠校验需要）、可直接当 `int` 返回值用、有调试信息；保留现有 memory 段**取值**不变 |
| 不选 `enum class` 的理由 | 错误码常作 `int` 返回/比较，`enum class` 处处要 `static_cast`；`Status`（P1/P2）再做强类型封装更合适 |
| `SUCCESS` 处理 | 全局宏 `SUCCESS` 名过于通用、易冲突，改为 `cabe::err::kSuccess = 0` |

### 决策-3：logger 用最简纯头宏 + `printf` 风格 `fprintf`（owner 指定）

| 维度 | 内容 |
|---|---|
| 备选 | `std::format` + `Emit` 模板 + `logger.cpp` / **纯头宏 + `fprintf`** |
| 本设计裁决 | **纯头宏 + `printf` 风格 `std::fprintf`**：沿用原 `logger.h`「纯头、宏」的最简形态，仅把空操作宏体换成单次 `fprintf` 到 stderr。无 `.cpp`、无模板、无 `std::format`。`fmt` 须为字符串字面量（与前缀拼接）；用 C++20 标准 `__VA_OPT__` 转发变参（不用 GNU `##__VA_ARGS__`，`-Wpedantic` 干净） |
| 取舍 | 牺牲 `std::format` 的强类型安全，改由 `-Wformat`（`-Wall` 含）编译期校验 `fprintf` 格式串 vs 变参；换取最简实现与纯头形态 |

### 决策-4：`cabe_common` 保持 INTERFACE（logger 纯头，无 `.cpp`）

| 维度 | 内容 |
|---|---|
| 背景 | 决策-3 选纯头宏方案后，common 下没有任何源文件，无需可编译库 |
| 本设计裁决 | `cabe_common` **保持 INTERFACE 头库**（M1 原状），`error_code.h`/`structs.h`/`logger.h` 全为纯头；`common/CMakeLists.txt` 不改动 |
| 说明 | P0M1 §12 曾预告"M3 若新增 logger.cpp 则升 STATIC"，现因 logger 纯头，该路径不触发 |

---

## 4. `error_code.h` 设计

### 4.1 段位划分（每段 1000 号，负值递减）

| 段 | 基址 | 占用范围 | 用途 |
|---|---|---|---|
| memory | `-100000` | `-100000 ~ -100999` | 内存 / 参数校验（现有，保留） |
| io | `-101000` | `-101000 ~ -101999` | I/O 后端 |
| index | `-102000` | `-102000 ~ -102999` | MetaIndex |
| wal | `-103000` | `-103000 ~ -103999` | WAL 写入 |
| engine | `-104000` | `-104000 ~ -104999` | Engine / Options / 生命周期 |
| wal_recovery | `-105000` | `-105000 ~ -105999` | 崩溃恢复 |

- 约定：码值从基址**向更负方向递减**编号（`base`、`base-1`…），段容量 1000，故每段可用 `base ~ base-999`。
- `kSuccess = 0`，与所有错误段（负值）天然不冲突。

### 4.2 设计示意

```cpp
#ifndef CABE_ERROR_CODE_H
#define CABE_ERROR_CODE_H

namespace cabe::err {

inline constexpr int kSuccess = 0;

// ---- 段基址与容量 ----
inline constexpr int kSegmentSize     = 1000;
inline constexpr int kMemoryBase      = -100000;
inline constexpr int kIoBase          = -101000;
inline constexpr int kIndexBase       = -102000;
inline constexpr int kWalBase         = -103000;
inline constexpr int kEngineBase      = -104000;
inline constexpr int kWalRecoveryBase = -105000;

// ---- 段位不重叠（编译期保证；相邻段恰好相距一个段容量，无缝且不交叠）----
static_assert(kMemoryBase - kSegmentSize == kIoBase);
static_assert(kIoBase     - kSegmentSize == kIndexBase);
static_assert(kIndexBase  - kSegmentSize == kWalBase);
static_assert(kWalBase    - kSegmentSize == kEngineBase);
static_assert(kEngineBase - kSegmentSize == kWalRecoveryBase);

// 段内编号辅助：第 n 个码（n ∈ [0, kSegmentSize)）
constexpr int InSeg(int base, int n) { return base - n; }

// ---- memory 段（保留现有取值）----
inline constexpr int kMemNullPointer = InSeg(kMemoryBase, 0);  // -100000
inline constexpr int kMemEmptyKey    = InSeg(kMemoryBase, 1);  // -100001
inline constexpr int kMemEmptyValue  = InSeg(kMemoryBase, 2);  // -100002
inline constexpr int kMemInsertFail  = InSeg(kMemoryBase, 3);  // -100003

// 每个码不得越段（编译期）
static_assert(kMemInsertFail > kMemoryBase - kSegmentSize);

// io / index / wal / engine / wal_recovery 段的具体码随各模块产生时补入（§1.3）。

}  // namespace cabe::err

#endif // CABE_ERROR_CODE_H
```

> 兼容性说明：现有 memory 段四个码的**数值**（-100000 ~ -100003）完全保留，仅由 `#define`
> 形态改为 `cabe::err::kXxx`。当前无调用点，改名零风险。

---

## 5. `logger.h` 设计（纯头宏 + stderr）

### 5.1 五级语义（补全现注释漏掉的 INFO）

| 级别 | 语义 |
|---|---|
| `DEBUG` | 正常业务路径的详细跟踪（如 key not found 这类预期分支，不算错误） |
| `INFO` | 正常关键事件（Open/Close 成功、设备就绪等） |
| `WARN` | 调用方编程错误（空 key、重复 Open 等） |
| `ERROR` | 系统级故障（磁盘 I/O 失败、内存耗尽） |
| `FATAL` | 内部不变式被破坏（double-release、CRC 与索引不一致等引擎 bug） |

### 5.2 输出格式

```
[LEVEL][tid][file:line] message
```
例：`[WARN][18342][engine_api.cpp:42] empty key rejected`
- `LEVEL`：定宽大写名（`DEBUG`/`INFO`/`WARN`/`ERROR`/`FATAL`）。
- `tid`：线程 id（Linux `gettid()`，无锁项目里用于区分 reactor 线程）。
- `file:line`：`__FILE__` 取 basename + `__LINE__`。

### 5.3 级别控制：`CABE_LOG_LEVEL`

- 环境变量 `CABE_LOG_LEVEL`，取值 `DEBUG|INFO|WARN|ERROR|FATAL`（大小写不敏感），**默认 `WARN`**。
- **首次使用时解析一次并缓存**（函数内 `static`，construct-on-first-use，C++11 线程安全，与 `cpu_features::GetFeatures()` 同模式，SIOF-safe）。
- 低于阈值的日志被丢弃；丢弃判断在宏内**短路**，避免无谓的参数格式化开销。

### 5.4 接口设计示意（全部在 `logger.h`，纯头）

```cpp
#pragma once
#include <cstdio>   // std::fprintf
#include <cstdlib>  // std::getenv
#include <cstring>  // std::strrchr
#include <unistd.h> // ::gettid

namespace cabe::log {

enum class Level : int { Debug = 0, Info, Warn, Error, Fatal };

inline const char* Name(Level lv) noexcept;        // "DEBUG".."FATAL"（switch + 末尾兜底 return）
inline Level       Threshold() noexcept;           // 函数内 static + IIFE 解析 CABE_LOG_LEVEL（首用一次）
inline bool        Enabled(Level lv) noexcept      // 级别比较（enum 转 int）
    { return static_cast<int>(lv) >= static_cast<int>(Threshold()); }
inline const char* Basename(const char* path) noexcept;  // strrchr('/') 取文件名

}  // namespace cabe::log

// 整行单次 fprintf；Enabled() 短路；fmt 为 printf 字面量；__VA_OPT__ 转发变参。
#define CABE_LOG_AT(lv, fmt, ...)                                              \
    do {                                                                       \
        if (::cabe::log::Enabled(lv))                                          \
            std::fprintf(stderr, "[%s][%ld][%s:%d] " fmt "\n",                 \
                         ::cabe::log::Name(lv), static_cast<long>(::gettid()), \
                         ::cabe::log::Basename(__FILE__),                      \
                         __LINE__ __VA_OPT__(, ) __VA_ARGS__);                 \
    } while (0)

#define CABE_LOG_DEBUG(fmt, ...) CABE_LOG_AT(::cabe::log::Level::Debug, fmt __VA_OPT__(, ) __VA_ARGS__)
// INFO / WARN / ERROR / FATAL 同理（仅级别不同）
```

- `Name` / `Threshold` / `Enabled` / `Basename` 全为 `logger.h` 内 `inline` 函数：纯头、无 `.cpp`；
  `inline` 函数的函数内 `static`（阈值缓存）跨翻译单元是同一实例，故"首用解析一次"在全程序成立。

### 5.5 线程安全

- **不引入 mutex / 自旋锁**（业务路径禁锁，D16）。整行用**单次 `std::fprintf`** 输出：C 标准要求
  stdio 函数线程安全，每次 `fprintf` 调用对该 `FILE` 原子加锁，故**整行不会与其他线程的行交织**。
- 这里用到的是 libc stdio 的内部锁，**不在无锁数据路径上**（日志非主路径），与 D16「业务路径无锁」不冲突。
- `Threshold()` 只读缓存，天然无竞争。

### 5.6 `FATAL` 不在 logger 内 abort

- `FATAL` 仅**输出**，不主动 `abort()`：logger 单一职责是"记日志"，是否终止进程由调用方 / `assert` /
  上层策略决定（避免日志库越权控制流程）。需要快速失败的点用 `assert` 或显式 `std::abort()`。

---

## 6. CMake：`cabe_common` 保持 INTERFACE（无连带改动）

logger 为纯头宏实现，common 下无任何源文件，故 `cabe_common` **维持 M1 的 INTERFACE 头库**，
`common/CMakeLists.txt` 与根 `CMakeLists.txt` **均无需改动**：

```cmake
# common/CMakeLists.txt（M3 不变，仍 INTERFACE）
add_library(cabe_common INTERFACE)
target_include_directories(cabe_common INTERFACE ${PROJECT_SOURCE_DIR})
target_link_libraries(cabe_common INTERFACE cabe_flags)
add_library(cabe::common ALIAS cabe_common)
```

- `error_code.h` / `structs.h` / `logger.h` 全为纯头，随 `cabe_common` 的 include 根暴露。
- P0M1 §12 曾预告"若新增 logger.cpp 则升 STATIC"——本里程碑因 logger 纯头，该路径不触发。

---

## 7. 关键设计决策

| # | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| M3-D1 | error_code / logger 进 `cabe::err` / `cabe::log`；`CABE_LOG_*` 宏名保留 | 全局 | 延续 M2 命名空间约定，避免全局污染 | **建议采纳，待终审**（§3 决策-1，与 M2-D1 一并） |
| M3-D2 | 错误码用 `inline constexpr int`，弃 `#define` | `#define` / `enum class` | 类型安全、可 `static_assert`、可直接作 int | 建议采纳（§3 决策-2） |
| M3-D3 | 六段每段 1000 号 + 相邻段等距的不重叠 `static_assert` | 不校验 / 运行期校验 | ROADMAP 退出条件「段位静态不重叠」；编译期零成本 | 锁定 |
| M3-D4 | logger 用纯头宏 + `printf` 风格 `fprintf`（无 `.cpp`/无 `std::format`） | `std::format`+模板+`.cpp` | owner 指定：沿用原 logger.h 最简纯头形态；`-Wformat` 把关格式串 | 锁定（owner 定） |
| M3-D5 | `CABE_LOG_LEVEL` 运行期控级，默认 `WARN`，首用解析缓存 | 编译期裁剪 | ROADMAP 指定 env 控级；construct-on-first-use 线程安全 | 锁定 |
| M3-D6 | 宏内 `Enabled()` 短路，过滤掉的日志不格式化 | 总是格式化再判级 | 热路径 DEBUG 日志零额外格式化开销 | 锁定 |
| M3-D7 | 整行单次 `std::fprintf`（stdio 内部锁保证整行原子） | 自管缓冲 + `write` / mutex | 最简；stdio 锁非业务路径，不违反 D16；整行不交织 | 锁定 |
| M3-D8 | `FATAL` 只输出、不 abort | FATAL 即 abort | logger 单一职责；终止由 assert/调用方决定 | 锁定（可议） |
| M3-D9 | `cabe_common` 保持 INTERFACE（logger 纯头，无 `.cpp`） | 升 STATIC | 纯头方案下 common 无源文件，无需可编译库 | 锁定（owner 定） |

---

## 8. 与 ROADMAP M3 一致性核对

| ROADMAP M3 要求 | 本设计 | 状态 |
|---|---|---|
| 六段（memory/io/index/wal/engine/wal_recovery），每段 1000 号 | §4.1 / §4.2 | ✅ |
| 现有 memory 段（-100xxx）保留 | §4.2（取值不变） | ✅ |
| logger.h stderr 最简实现（~100 行） | §5（logger.h 纯头，约 100 行） | ✅ |
| 环境变量 `CABE_LOG_LEVEL`（默认 WARN）控级 | §5.3 | ✅ |
| 输出格式 `[LEVEL][tid][file:line] message` | §5.2 | ✅ |
| 禁止全空操作 | §5 实装 stderr | ✅ |
| 退出：demo 五级日志 + 段位不重叠编译期断言 | §10 | ✅ |
| 命名空间 / 错误码形态 | 进 `cabe::`、改 `constexpr` | ⚠️ 超出字面，待终审（§3） |

---

## 9. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 多线程日志交织 | 并发写 stderr 可能乱序/交错 | 整行单次 `std::fprintf`，stdio 内部锁保证整行原子（§5.5）；行间顺序非强保证，可接受 |
| `printf` 格式串与参数不匹配 | `fprintf` 不如 `std::format` 类型安全 | `fmt` 为字面量 + `-Wformat`（`-Wall` 含）编译期校验格式串 vs 变参，不匹配即告警 |
| `CABE_LOG_LEVEL` 运行期变更不生效 | 阈值首用即缓存 | 文档说明"进程启动时读一次"；如需热更新另设接口（非 M3） |
| logger 纯头 → 每个 include 者带 `<cstdio>`/`<unistd.h>` | 头依赖略增 | 影响小（Linux only、stdio 常用）；省去 `.cpp` 与库拓扑变更 |
| `gettid()` 可移植性 | Linux 专有 | 项目本就 Linux only（D：跨平台排除） |

---

## 10. 退出条件与验证步骤

1. `error_code.h` / `logger.h`（纯头）经一个 include 它们、调用日志宏的 demo，在 GCC 15 / Clang 20 下编译通过（扩展关闭、`-Wall -Wextra -Wpedantic`、零警告）。注：当前无工程 TU 引用这两个头，故须借 demo 触发编译与 `static_assert`。
2. **段位不重叠 `static_assert` 全部通过**（编译期）。
3. **小 demo**（不计入交付，验证后移除）：依次 `CABE_LOG_DEBUG..FATAL` 各打一条；
   - 默认（不设 env）：只见 `WARN/ERROR/FATAL` 三行；
   - `CABE_LOG_LEVEL=DEBUG`：五行全见；
   - `CABE_LOG_LEVEL=ERROR`：只见 `ERROR/FATAL`；
   - 行格式匹配 `[LEVEL][tid][file:line] message`。
4. memory 段取值回归：`kMemNullPointer==-100000` … `kMemInsertFail==-100003`（可 `static_assert`）。
5. 工程双工具链构建通过（`cabe_common` 保持 INTERFACE、`cabe_util` 不受影响）。

> **不含**：error_code / logger 的正式单元测试（M5）、`Status` 封装（P1/P2）。

---

## 11. 对下游里程碑的接口承诺

| 里程碑 | M3 提供的接入点 |
|---|---|
| M4 | hash 模块可用 `CABE_LOG_*` 日志、`cabe::err` 错误码 |
| M5 | error_code（段位不重叠）、logger（级别过滤 / 格式）可被单测覆盖 |
| P1 | `Engine` 用 `cabe::err::*` 作返回码雏形；各路径用日志宏；`Status`（P2）在错误码空间上封装 |
| P3+ | io / index 段基址已就位，各后端 / 索引补具体码即可，不再动段划分 |
| P5 | wal / wal_recovery 段就位，WAL 与恢复补具体码 |
