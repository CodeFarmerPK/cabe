# Cabe P0-M1 设计：项目骨架与构建系统

> 本文是 P0 阶段第 1 个里程碑（M1）的详细设计定稿。聚焦"让工程可配置、可构建"，
> 不含任何业务逻辑、第三方依赖与测试内容。本里程碑的设计结论最终将在 M7 收敛进
> `doc/P0/P0_infra_design.md` 的"构建系统"一节。
>
> **本文取代**早期扁平命名的 `doc/p0_m1_skeleton_design.md`（迁入分阶段目录并定稿）。
> 命名与目录约定见 §13。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P0 / M1 |
| 状态 | **完成稿（待 owner 终审）** —— 技术决策已全部裁定，仅余 §3 两处与路线图字面有出入的偏差待终审 |
| 上游依赖 | 无（M1 是 P0 起点） |
| 下游依赖本里程碑 | M2 / M3 / M4 均依赖 M1 产出的可构建骨架 |
| 关联架构决策 | D20–D23（CMake 选项预留）、贯穿约束（仅 Linux / C++20 / GCC 15+ / Clang 20+） |
| 退出判定 | `cmake -S . -B build && cmake --build build` 在 GCC 15 与 Clang 20 下均通过 |
| 不生成代码 | 本里程碑当前阶段只产出**设计稿**；CMake 片段为设计示意，落地实现待终审通过后单独提交 |

---

## 1. 目标与范围

### 1.1 目标

把当前"只有零散源文件、无任何构建系统"的仓库，升级为一个**可配置、可构建**的
现代 CMake 工程：

1. 现有 `util/` 与 `common/` 纳入构建，产出可被后续阶段链接的库目标。
2. 平台 / 工具链 / C++ 标准三道**强校验**前置，错误环境在配置期即快速失败（fail-fast）。
3. 预留 `CABE_IO_BACKEND` / `CABE_META_INDEX` / `CABE_SANITIZER` 三个 CMake 选项。
4. 建立清晰的现代 CMake 目标拓扑（接口属性库 + 静态库 + 头库），
   为 M2–M7 提供稳定的接入点。

### 1.2 交付范围（本里程碑产出）

- 根 `CMakeLists.txt`
- `common/CMakeLists.txt`、`util/CMakeLists.txt`
- `.gitignore`（构建目录 / IDE 目录）
- 三个预留选项 + 构建类型默认 + 警告策略

### 1.3 推迟范围（明确推迟，标注落点）

| 推迟项 | 落点 | 原因 |
|---|---|---|
| GTest / google-benchmark 接入，`test/` `bench/` 子目录内容 | **M5** | 框架接入属 M5；M1 仅留接入缝（见 §3 偏差-2、§6.8） |
| 第三方依赖 xxhash | **M4** | hash 模块在 M4 引入 |
| 第三方依赖 liburing | **P4** | io_uring 后端阶段 |
| `CABE_SANITIZER` 的脚本（`run-tests.sh`）与 CI 矩阵 | **M6** | M1 仅声明并应用编译开关（见 §3 偏差-1） |
| `CABE_IO_BACKEND` / `CABE_META_INDEX` 的真实编译期分派 | **P3** | 此时尚无后端 / 索引源码（见 §8 M1-D3） |
| `CMakePresets.json` | M6（可选） | 与 `run-tests.sh` 一并设计 |
| `engine/` `io/` `index/` `wal/` `reactor/` 子目录 | 各自起始阶段（P1/P3/P5/P7） | 当前无源码 |
| 源码 schema 改造（`structs.h` 等） | **M2** | M1 一行源码不改 |
| CMake 必需 POSIX 头探测（`sys/mman.h` / `fcntl.h` / `unistd.h`） | **P1** | M1 的 util/common 不用 O_DIRECT/mmap/pread，引擎 I/O 起于 P1 |
| CMake 内核版本软校验（6.16+ `WARNING`） | **P4** | 内核新特性门槛由 io_uring 阶段触发 |
| CMake 发行版硬校验（仅 Fedora 43） | **不加（设计决策）** | setup-dev.sh 已做；CMake 已直接校验编译器版本（GCC15/Clang20），比发行版校验更精准，且不误伤兼容 Linux 环境 |

---

## 2. 现状盘点（读码结论）

构建系统需要精确建立在现有文件之上，以下为 M1 设计所依赖的事实：

**可编译单元（`.cpp`）**：仅 2 个
- `util/crc32.cpp`
- `util/cpu_features.cpp`

**头文件**：
- `common/error_code.h`、`common/logger.h`、`common/structs.h`（全部为头文件，无 `.cpp`）
- `util/crc32.h`、`util/cpu_features.h`、`util/util.h`（`util.h` 为纯头实现）

**包含关系（决定 include 根）**：
- `util/crc32.h` → `#include "common/structs.h"` —— **需要工程根在搜索路径上**
- `util/crc32.cpp` → `#include "crc32.h"`、`#include "cpu_features.h"` —— 同目录兄弟，裸名
- `util/cpu_features.{h,cpp}` —— 自含，仅依赖系统头

**工具链特性使用（决定编译开关策略）**：
- `crc32.cpp` 使用 `[[gnu::target("sse4.2")]]` 函数属性 + `<nmmintrin.h>` 的
  `_mm_crc32_u64` / `_mm_crc32_u8`，并在运行时通过 `cpu::HasSSE42()` 分派。
  → **关键约束**：绝不可在编译期加全局 `-msse4.2`，否则破坏"软件兜底 +
  运行时选择"的可移植设计（见 §8 M1-D7）。
- `cpu_features.cpp` 使用 `<cpuid.h>` / `__get_cpuid`（x86）或 `<sys/auxv.h>`（ARM 占位）。

**既有平台兜底**：`common/structs.h` 顶部已有 `#if !defined(__linux__) #error`。
M1 在 CMake 层再加一道（配置期 fail-fast），与源码级兜底互补。

**环境基线（README + `scripts/setup-dev.sh` 已声明，M1 须与之一致）**：
- CMake 3.30+、GCC 15 / Clang 20、C++20、Ninja、默认 Release。

> 备注：`util/util.h` 注释提到 `KeyMeta.createdAt`，而 `structs.h` 现为 `ChunkMeta`，
> 二者命名不一致 —— 属 M2 schema 改造范畴，**M1 不触碰**，此处仅记录不作处理。

---

## 3. 待 owner 终审的偏差（review 时优先裁决）

本设计在两处与路线图（ROADMAP）的字面表述存在出入。二者技术上均已做出明确裁决并采纳，
但因涉及里程碑范围划分，列于此处供终审一票定夺。其余决策见 §8 决策表。

### 偏差-1（对应 M1-D1）：`CABE_SANITIZER` 在 M1 即"声明 + 校验 + 应用编译开关"

| 维度 | 内容 |
|---|---|
| 路线图字面 | P0 范围把 "Sanitizer 矩阵 + CI 工作流" 归在 **M6** |
| 本设计裁决 | **采纳**：M1 即声明选项、校验合法值、并应用编译/链接开关；M6 只剩脚本与 CI 矩阵 |
| 理由 | 开关应用仅约 8 行，提前到位可让 M2–M5 本地即用 ASAN / TSAN 调试；与路线图不冲突 —— M6 的"实装"指 `run-tests.sh` 脚本 + CI 矩阵 + 可选 presets，而非编译开关本身 |
| 回退代价 | 极低：若终审要求 M1 仅声明不应用，从 `cabe_flags` 删去约 8 行开关即可 |

### 偏差-2（对应 M1-D2）：`test/` `bench/` 子目录推迟到 M5

| 维度 | 内容 |
|---|---|
| 路线图字面 | P0/M1 列了"子目录 CMake（`common/` / `util/` / `test/` / `bench/`）" |
| 本设计裁决 | **采纳推迟**：M1 只做 `enable_testing()` + `CABE_BUILD_TESTS/BENCH` 选项 + `EXISTS` 守卫的 `add_subdirectory`；`test/` `bench/` 内容到 M5 |
| 理由 | 二者实际内容（`find_package(GTest)` / google-benchmark）属 M5 范畴，M1 无任何测试源码；M1 退出条件只要求生产代码可构建。先建空 placeholder 目录无意义，反而引入"空壳 CMake" |
| 回退代价 | 低：若终审要求 M1 即建占位目录，新增两个仅含注释的 `CMakeLists.txt` 即可，§6.8 的守卫逻辑无需改动 |

> 这两处一旦终审反转，受影响章节为 §1.2 / §1.3、§4 文件清单、§6.7、§6.8、§8 决策表、§9 一致性核对、§11 验证清单。

---

## 4. 产出文件清单

| 路径 | 动作 | 摘要 |
|---|---|---|
| `CMakeLists.txt` | **新建** | 根：project / 三道校验 / 选项 / flags 库 / add_subdirectory |
| `common/CMakeLists.txt` | **新建** | 定义 `cabe_common`（接口头库） |
| `util/CMakeLists.txt` | **新建** | 定义 `cabe_util`（STATIC，2 个 .cpp） |
| `.gitignore` | **新建** | 见 §7 |
| 所有现有 `.h` / `.cpp` | **不改** | M1 不动任何源码 |

> `test/CMakeLists.txt`、`bench/CMakeLists.txt` **不在 M1 产出**，推迟至 M5（见 §3 偏差-2）。

---

## 5. 构建系统架构

### 5.1 目标拓扑

```
            cabe_flags  (INTERFACE)
            ├─ cxx_std_20 (EXTENSIONS OFF)
            ├─ 警告: -Wall -Wextra -Wpedantic [可选 -Werror]
            └─ sanitizer 编译/链接开关  (按 CABE_SANITIZER)
                  ▲  cabe_common 以 INTERFACE 方式链接
                  │
            cabe_common (INTERFACE)
            ├─ include 根 = ${PROJECT_SOURCE_DIR}
            └─ 头: common/{error_code,logger,structs}.h
                  ▲  cabe_util 以 PUBLIC 方式链接
                  │
            cabe_util   (STATIC)   →  别名 cabe::util
            └─ util/crc32.cpp + util/cpu_features.cpp
```

链接关系自下而上、且严格匹配目标类型：
- `cabe_common` 是 INTERFACE 库，只能 INTERFACE 链接 `cabe_flags`；
- `cabe_util` 是 STATIC 库，以 PUBLIC 链接 `cabe_common`，从而把"include 根 + 标准/警告/sanitizer"
  整条传递性属性向上游消费者继承。

任何未来消费者（P1 的 engine 等）只需 `target_link_libraries(xxx PRIVATE cabe::util)`
即自动继承上述全部传递性属性。

### 5.2 各目标职责

| 目标 | 类型 | 职责 | 传递给消费者 |
|---|---|---|---|
| `cabe_flags` | INTERFACE | 统一编译契约：C++ 标准、警告、sanitizer | 编译/链接开关 |
| `cabe_common` | INTERFACE | 头库 + 单一 include 根 | `-I${PROJECT_SOURCE_DIR}` + flags |
| `cabe_util` | STATIC | CRC32C / CPU 特性检测的实现 | 链接库 + common/flags |

### 5.3 include 约定

- **单一 include 根 = 工程根**（`${PROJECT_SOURCE_DIR}`），由 `cabe_common` 以
  INTERFACE 方式向上传递。本项目不做安装（install），故无需 `$<BUILD_INTERFACE:>`
  之类 generator 表达式包裹，直接用 `${PROJECT_SOURCE_DIR}` 即可。
- **外部消费**：一律路径限定，`#include "util/crc32.h"`、`#include "common/structs.h"`。
- **同目录兄弟**：可用裸名，`#include "crc32.h"`（C++ 对 `"..."` 形式优先搜索
  当前源文件所在目录，与 `-I` 无关）。现有 `crc32.cpp` 即此模式，无需改动。
- 理由：零歧义、无需为每个子目录单独 `target_include_directories`，符合现有代码习惯。

### 5.4 为什么 `cabe_util` 用 STATIC

- 单机引擎，最终形态是静态链接的可执行/库，无动态插件需求。
- STATIC 便于后续整库 LTO、避免 SHARED 的 PIC / 符号可见性复杂度。
- 暂不用 OBJECT 库：当存在"同一组对象被多个 target 复用且想避免重复编译"
  的明确需求时（可能在 P3 多后端出现）再升级，当前无此需求。

---

## 6. 根 CMakeLists 设计（逐段，含示意片段）

> 下列片段为**设计示意**，用于固定决策与结构；最终实现代码在终审通过后单独提交。

### 6.1 minimum / project

```cmake
cmake_minimum_required(VERSION 3.30)          # 与 setup-dev.sh 声明一致
project(cabe VERSION 0.0.1 LANGUAGES CXX)
```
- `3.30`：与环境基线对齐；Fedora 43 自带 ≥ 3.31，CI 容器满足。目标发行版固定，
  取较新最低版减少兼容包袱（见 §8 M1-D6）。
- **工程标识符用小写 `cabe`**（与仓库目录 `cabe/`、`cabe_*` target 前缀一致）；产品展示名
  `Cabe`（文档标题、注释、面向用户的 `message` 文案）仍用大写，二者各自内部统一（见 §8 M1-D14）。

### 6.2 平台校验（仅 Linux）

```cmake
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Cabe only supports Linux (target: Fedora 43). See README.md.")
endif()
```
与 `structs.h` 的 `#error` 互补：CMake 在配置期就拦截，IDE 误配也能及时报错。

### 6.3 工具链校验（GCC ≥ 15 / Clang ≥ 20）

```cmake
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
        message(FATAL_ERROR "Cabe requires GCC >= 15 (found ${CMAKE_CXX_COMPILER_VERSION}).")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
        message(FATAL_ERROR "Cabe requires Clang >= 20 (found ${CMAKE_CXX_COMPILER_VERSION}).")
    endif()
else()
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. Use GCC 15+ or Clang 20+.")
endif()
```
- 版本门理由：项目统一锁定 Fedora 43 工具链，确保 `<format>` / ranges / concepts /
  `<chrono>` 日历等 C++20 库特性行为一致，并为后续可能的 C++23 局部特性留空间。
- 只认 `GNU` / `Clang`（Linux 下不会出现 `AppleClang`）。

### 6.4 C++ 标准

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)                 # 用 -std=c++20，而非 gnu++20
```
- `EXTENSIONS OFF` 安全性论证：现有代码用到的"GNU 味"特性是
  ① `[[gnu::target("sse4.2")]]`（标准属性语法承载的厂商属性，标准模式下可用）、
  ② `<cpuid.h>` / `__get_cpuid`（编译器自带头/内建，不受 `-std` 影响）。
  二者**均不依赖语言扩展**，故 `OFF` 不会破坏现有代码。
  → M1 须在 GCC 15 与 Clang 20 下各实测一次确认（见 §11 验证清单第 6 项）。

### 6.5 默认构建类型

```cmake
get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
```
单配置生成器（默认 Ninja）未指定时落 Release，与 README 一致；多配置生成器不强加。

### 6.6 预留选项

```cmake
# 立即生效（M1 即应用编译开关，见 §3 偏差-1）
set(CABE_SANITIZER "none" CACHE STRING "Sanitizer: none|address|thread|undefined")
set_property(CACHE CABE_SANITIZER PROPERTY STRINGS none address thread undefined)

# 仅声明 + 校验，真实分派在 P3
set(CABE_IO_BACKEND "sync" CACHE STRING "I/O backend: sync|io_uring|spdk")
set_property(CACHE CABE_IO_BACKEND PROPERTY STRINGS sync io_uring spdk)
set(CABE_META_INDEX "hashmap" CACHE STRING "Meta index: hashmap|bplustree")
set_property(CACHE CABE_META_INDEX PROPERTY STRINGS hashmap bplustree)

# 构建开关（默认 OFF，M5 起置 ON）
option(CABE_BUILD_TESTS "Build unit tests"   OFF)
option(CABE_BUILD_BENCH "Build benchmarks"   OFF)
option(CABE_WERROR      "Treat warnings as errors" OFF)
```

**合法值校验**：每个 string 选项均校验合法值（非法值 `FATAL_ERROR`），并已设 `STRINGS`
供 `ccmake` / GUI 下拉。三个 string 选项的统一校验示意：

```cmake
function(cabe_validate_choice var)
    set(_allowed ${ARGN})
    if(NOT "${${var}}" IN_LIST _allowed)
        message(FATAL_ERROR "Invalid ${var}='${${var}}'. Allowed: ${_allowed}")
    endif()
endfunction()

cabe_validate_choice(CABE_SANITIZER  none address thread undefined)
cabe_validate_choice(CABE_IO_BACKEND sync io_uring spdk)
cabe_validate_choice(CABE_META_INDEX hashmap bplustree)
```

**非默认值提示**：`CABE_IO_BACKEND` / `CABE_META_INDEX` 在 M1 **不影响任何编译产物**，
仅占位 + 校验；P3 起接编译期分派。对非默认值打 `STATUS` 提示其生效阶段，避免使用者误以为已生效：

```cmake
if(NOT CABE_IO_BACKEND STREQUAL "sync")
    message(STATUS "CABE_IO_BACKEND='${CABE_IO_BACKEND}' recorded; real dispatch lands in P3.")
endif()
if(NOT CABE_META_INDEX STREQUAL "hashmap")
    message(STATUS "CABE_META_INDEX='${CABE_META_INDEX}' recorded; real dispatch lands in P3/P9.")
endif()
```

### 6.7 `cabe_flags` 接口库

```cmake
add_library(cabe_flags INTERFACE)
target_compile_features(cabe_flags INTERFACE cxx_std_20)
target_compile_options(cabe_flags INTERFACE -Wall -Wextra -Wpedantic)
# 三条"等于未定义行为"的告警默认升级为硬错误（吸收自既往单文件 CMake，见 §8 M1-D13）
target_compile_options(cabe_flags INTERFACE
    -Werror=return-type
    -Werror=uninitialized
    -Werror=implicit-fallthrough)
if(CABE_WERROR)
    target_compile_options(cabe_flags INTERFACE -Werror)
endif()
if(NOT CABE_SANITIZER STREQUAL "none")
    target_compile_options(cabe_flags INTERFACE -fsanitize=${CABE_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(   cabe_flags INTERFACE -fsanitize=${CABE_SANITIZER})
    message(STATUS "Sanitizer enabled: ${CABE_SANITIZER} (use Debug/RelWithDebInfo for best stack traces)")
endif()
```
- 警告基线 `-Wall -Wextra -Wpedantic` 始终开。
- **三条选择性 `-Werror=`**（`return-type` / `uninitialized` / `implicit-fallthrough`）**默认即开**：
  它们一旦触发基本等于未定义行为，属"代码写错了"而非风格问题，应无条件拦截（见 §8 M1-D13）。
- 全量 `-Werror` 经 `CABE_WERROR` 开关（默认 OFF），避免编译器升级引入新告警直接卡死本地/CI（见 §8 M1-D10）。
- **不采用** `-Wno-unused-parameter`：它会掩盖真实的未用参数问题；将来 backend 接口若有占位参数，用
  `[[maybe_unused]]` 或局部 `(void)` 处理更精准。
- 所有警告/错误开关一律施加在 `cabe_flags`（INTERFACE target）上，**只作用于本项目 target**，
  不会污染 `FetchContent` 引入的第三方代码（GTest / google-benchmark）。
- **sanitizer 附带 `-g`**：报告符号化（`源文件:行号`）依赖调试信息，默认 Release 不含 `-g`，故显式补上；
  并打 `STATUS` 提示 sanitizer 构建配 `Debug` / `RelWithDebInfo` 获得最佳栈回溯（见 §8 M1-D1）。

### 6.8 add_subdirectory 顺序

```cmake
enable_testing()                     # 便宜，提前就绪；具体 test 目标 M5 再加
add_subdirectory(common)
add_subdirectory(util)
if(CABE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)           # M5 起存在
endif()
if(CABE_BUILD_BENCH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt")
    add_subdirectory(bench)          # M5 起存在
endif()
```

`EXISTS` 守卫使本段在 M1（无 `test/` `bench/`）与 M5（有）下都成立，无需将来回改根 CMake。

### 6.9 子目录 CMake 示意

`common/CMakeLists.txt`：
```cmake
add_library(cabe_common INTERFACE)
target_include_directories(cabe_common INTERFACE ${PROJECT_SOURCE_DIR})
target_link_libraries(cabe_common INTERFACE cabe_flags)
add_library(cabe::common ALIAS cabe_common)
```

`util/CMakeLists.txt`：
```cmake
add_library(cabe_util STATIC
    crc32.cpp
    cpu_features.cpp
    # hash.cpp        # ← M4 在此加入 xxh3 路由 hash
)
target_link_libraries(cabe_util PUBLIC cabe_common)   # 含 cabe_flags 传递
add_library(cabe::util ALIAS cabe_util)
```

---

## 7. `.gitignore` 设计

仓库当前已有未跟踪的 `cmake-build-debug/`、`.idea/`，需收敛（见 §8 M1-D11）。内容：

```gitignore
# 构建产物
/build*/
/cmake-build-*/
/out/

# IDE / 编辑器
/.idea/
/.vscode/
*.user

# 工具缓存
/.cache/
# clangd / IDE 软链到根目录的 compile_commands.json（真实副本在构建目录内，已随 build*/ 忽略）
compile_commands.json
```

> 说明：`.gitignore` 不支持行内注释（`#` 仅在行首才表示注释），故 `compile_commands.json`
> 的说明必须单独成行。该文件若被 IDE / clangd 软链到根目录则需忽略；真正的副本始终在
> 构建目录内，随构建目录一并被忽略。

---

## 8. 关键设计决策

> 编号沿用 ROADMAP 的 `D` 风格，前缀 `M1-` 表示本里程碑内决策。与路线图字面有出入的
> M1-D1 / M1-D2 详见 §3。

| # | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| M1-D1 | `CABE_SANITIZER` 在 M1 即声明 + 校验 + 应用编译开关 | 仅声明、M6 才接开关 | 见 §3 偏差-1 | **建议采纳，待终审** |
| M1-D2 | `test/` `bench/` 推迟到 M5，M1 只留接缝 | M1 即建占位目录 | 见 §3 偏差-2 | **建议采纳，待终审** |
| M1-D3 | `CABE_IO_BACKEND` / `CABE_META_INDEX` 在 M1 仅声明 + 校验，不接分派 | M1 即写编译期分支 | 当前无后端/索引源码，接分派无对象可选 | 锁定 |
| M1-D4 | 单一 include 根 = 工程根 | 每子目录各自暴露 include dir | 现有 `crc32.h → "common/structs.h"` 即此假设；零歧义 | 锁定 |
| M1-D5 | `cabe_util` 用 STATIC | OBJECT / SHARED | 单机静态链接、利于 LTO、避免 PIC 复杂度 | 锁定 |
| M1-D6 | `cmake_minimum_required(3.30)` | 3.20 等更低 | 与 setup-dev / README 对齐；目标发行版固定，取较新最低版 | 锁定（可议） |
| M1-D7 | **不加全局 `-msse4.2` 等 ISA 开关**，保留 `[[gnu::target]]` + 运行时分派 | 全局 `-msse4.2` | 全局开关会让二进制在无 SSE4.2 的 CPU 上 SIGILL，破坏软件兜底设计 | 锁定 |
| M1-D8 | `CXX_EXTENSIONS OFF`（`-std=c++20`） | `gnu++20` | 现有 GNU 特性均不依赖语言扩展（§6.4 论证）；更严格更可移植 | 锁定（M1 实测确认） |
| M1-D9 | 单配置默认 `Release` | 不设默认 | 与 README 一致 | 锁定 |
| M1-D10 | `-Werror` 默认 OFF，经 `CABE_WERROR` 开 | 默认 ON | 避免编译器升级新告警卡死；CI 可显式开 | 锁定（可议） |
| M1-D11 | 引入 `.gitignore`（构建/IDE 目录） | 不加 | 仓库当前有未跟踪 `cmake-build-debug/`、`.idea/`，需收敛 | 锁定 |
| M1-D12 | 阶段设计稿采用分阶段子目录命名（§13） | 沿用扁平 `pN_xxx_design.md` | 里程碑数量多，分目录便于检索与归档 | 锁定（本轮由 owner 确立） |
| M1-D13 | `return-type` / `uninitialized` / `implicit-fallthrough` 三条 `-Werror=` 默认开 | 不升级 / 全量 `-Werror` 默认开 | 三者触发即近似未定义行为，属"写错了"而非风格；以 INTERFACE target 施加，不污染第三方 | 锁定（吸收自既往单文件 CMake） |
| M1-D14 | 工程标识符 `project(cabe)` 用小写；产品展示名 `Cabe` 保留大写 | 全大写 / 全小写一刀切 | 标识符与仓库目录、`cabe_*` target 前缀一致；展示名跟随 README 标题；各自内部统一 | 锁定（本轮 review 修正） |

---

## 9. 与 ROADMAP / README 一致性核对

| ROADMAP M1 要求 | 本设计 | 状态 |
|---|---|---|
| 根 + 子目录 CMake | §4 / §6 / §6.9 | ✅ |
| 编译器版本检查 GCC15/Clang20 | §6.3 | ✅ |
| C++20 标准 | §6.4 | ✅ |
| 预留 `CABE_IO_BACKEND`/`CABE_META_INDEX`/`CABE_SANITIZER` | §6.6 | ✅ |
| 现有 `util/*`、`cpu_features`、`crc32` 纳入 build | §6.9 `cabe_util` | ✅ |
| 子目录含 `test/` `bench/` | 推迟到 M5（§3 偏差-2） | ⚠️ **偏差，待终审** |
| 退出条件 configure+build 双工具链通过 | §11 | ✅ |
| README 默认 Release + Ninja | §6.5 | ✅ |
| README 默认 sync 后端 + hashmap 索引 | §6.6 默认值 | ✅ |
| Sanitizer 矩阵归在 M6 | M1 提前应用编译开关，脚本/CI 仍在 M6（§3 偏差-1） | ⚠️ **偏差，待终审** |

---

## 10. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| Clang 20 + 标准库 | Linux 下 Clang 默认配 libstdc++（非 libc++） | M1 用系统默认 libstdc++，不显式指定；若未来需 libc++ 单列选项 |
| `[[gnu::target]]` 在 `-std=c++20` 下行为 | 理论可用，需实证 | §11 验证第 6 项：GCC 与 Clang 各编一次 `crc32.cpp` |
| `-Werror` 脆性 | 编译器升级引入新告警 | 默认 OFF（M1-D10） |
| 生成器假设 | README 用 Ninja，但不应强制 | M1 不写死生成器，Make 亦可 |
| CMake 3.30 在 CI 容器可用性 | Fedora 43 容器满足 | setup-dev.sh `--ci` 已装 cmake |

---

## 11. 退出条件（DoD）与验证步骤

**配置 + 构建（两套工具链）**：
```bash
# GCC 15
cmake -S . -B build-gcc   -G Ninja -DCMAKE_CXX_COMPILER=g++
cmake --build build-gcc
# Clang 20
cmake -S . -B build-clang -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build-clang
```

**验证清单**：
1. 两套工具链 configure + build 均成功，产出 `libcabe_util.a`。
2. 非 Linux（或伪造 `CMAKE_SYSTEM_NAME`）配置时 `FATAL_ERROR`。
3. 低版本编译器（如 GCC 14）配置时 `FATAL_ERROR` 并提示所需版本。
4. `-DCABE_SANITIZER=bogus` 等非法值 `FATAL_ERROR`；合法值 `address` 能构建出带
   ASAN 的库。
5. cache 中 `CABE_IO_BACKEND` / `CABE_META_INDEX` / `CABE_SANITIZER` 可见且带 STRINGS；
   非默认值有 `STATUS` 提示。
6. **`-std=c++20`（扩展关闭）下 `crc32.cpp` 在 GCC 与 Clang 均编译通过**（验证 M1-D8）。
7. 编译产物中无全局 SSE4.2 指令污染（`crc32.cpp` 外的编译单元不含 crc32 指令）——
   验证 M1-D7；可选，通过 `objdump` 抽查。

**不含**：单元测试（M5）、CI 矩阵（M6）。M1 验证为人工本地双工具链跑通。

> 可选：M1 期间可临时加一个不安装的 `cabe_smoke` 可执行（调用 `cabe::util::CRC32`
> 与 `cpu::HasSSE42()`）以证明链接闭环，验证后即移除，**不计入交付物**。

---

## 12. 对下游里程碑的接口承诺

| 里程碑 | M1 提供的接入点 |
|---|---|
| M2 | 不动 schema；保证 `structs.h` 在 include 根下可被 `cabe_util` 引用 |
| M3 | `error_code.h` / `logger.h` 仍为头；M3 若新增 `logger.cpp`，复用 `util/CMakeLists` 的库写法，或并入 `cabe_common` → 可编译库 |
| M4 | `util/CMakeLists.txt` 已留 `hash.cpp` 加入点注释（§6.9） |
| M5 | `enable_testing()` 已就绪；`CABE_BUILD_TESTS/BENCH` 选项与 `EXISTS` 守卫已在位，M5 只需放入 `test/` `bench/` 的 CMake 与源码 |
| M6 | sanitizer 编译开关已就位（M1-D1），M6 只加 `run-tests.sh` + CI 矩阵 + 可选 presets |

---

## 13. 文档命名与目录约定

本里程碑同时确立 P0 起全项目设计稿的目录与命名约定（M1-D12）：

- **按阶段分目录**：`doc/P0/`、`doc/P1/`…，每阶段一个目录。
- **里程碑级设计稿**：`P<阶段>M<里程碑>_<主题>_design.md`，如本文 `P0M1_skeleton_design.md`、
  后续 `P0M2_schema_design.md`、`P0M3_errorcode_logger_design.md` 等。
- **阶段收敛文档**：`doc/P<n>/P<n>_infra_design.md` 之类（如 P0 的 `doc/P0/P0_infra_design.md`，
  即路线图中提到的 `p0_infra_design.md` 在新约定下的落点）。
- **阶段索引**：每个阶段目录含 `README.md`，列出本阶段里程碑文档清单与状态。

> 此约定是对路线图 §六 中 `doc/pN_xxx_design.md` 扁平命名的细化（改为分阶段子目录），
> 不改变其语义。落工程时，README / ROADMAP 中对 `doc/p0_infra_design.md` 的引用是否
> 同步更新路径，由 owner 在 M7 收敛时统一处理，本里程碑不擅改这两份文件。
