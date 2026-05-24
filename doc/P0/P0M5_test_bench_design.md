# Cabe P0-M5 设计：测试与 bench 框架接入 + util/common 覆盖

> 本里程碑接入 GTest（单元测试）与 google-benchmark（微基准），把 M1–M4 的工具库 / schema /
> 错误码 / 日志的行为用正式用例覆盖（行覆盖率 ≥ 80%），并把一路 smoke 验证过的结论（CRC32C
> 已知向量、`Hash("")` 基准、`BlockId` 编解码、段位不重叠、日志格式/过滤、hash 分布）固化下来。
> **本文为详细设计，暂不生成代码**；其中 C++/CMake 片段均为设计示意。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P0 / M5 |
| 状态 | **✅ 已锁定（P0M7 收敛）** |
| 上游依赖 | M1（`enable_testing`/`CABE_BUILD_TESTS|BENCH`/`EXISTS` 守卫）、M2/M3/M4（被测对象） |
| 下游依赖本里程碑 | M6（本地 ASAN / TSAN / UBSAN / Release 组合矩阵复用这些测试）、M7（bench 基线归档）、P1+（测试基建复用） |
| 关联约束 | ROADMAP P0/M5：GTest+benchmark 接入、`test/util` `test/common` `bench/util`、覆盖率 ≥ 80%、`ctest` 全绿 |
| 退出判定 | `ctest` 全绿；`util`+`common` 行覆盖率 ≥ 80%；双工具链下测试与 bench 均可构建运行 |

---

## 1. 目标与范围

### 1.1 目标
1. CMake 接入 GTest（`find_package` 优先、`FetchContent` 兜底）与 google-benchmark（同策略）。
2. `test/util`、`test/common` 单元测试覆盖 M1–M4 全部模块；`bench/util` 建立 crc32/hash 微基准骨架。
3. 覆盖率工具接入（双工具链：GCC gcov / Clang llvm-cov）+ `coverage` target，达到 util+common 行覆盖 ≥ 80%。

### 1.2 交付范围（本里程碑产出）
1. `test/CMakeLists.txt`、`bench/CMakeLists.txt`（M1 的 `EXISTS` 守卫此前已就位，本里程碑落实其内容）。
2. `test/util/{crc32,hash,util,cpu_features}_test.cpp`、`test/common/{structs,error_code,logger}_test.cpp`。
3. `bench/util/{crc32,hash}_bench.cpp`。
4. 覆盖率：根 `CMakeLists.txt` 加 `CABE_COVERAGE` 选项 + `coverage` 自定义 target。
5. **可测性改造（连带，见 §3 决策-2、§7）**：为测「crc32 软/硬一致性」与「logger 级别过滤」给 `crc32`/`logger` 开测试钩子。

### 1.3 推迟范围
| 推迟项 | 落点 | 原因 |
|---|---|---|
| 本地 ASAN / TSAN / UBSAN / Release 组合矩阵（双工具链）+ 覆盖率脚本 | **M6** | M5 只保证测试存在且本地全绿；矩阵与脚本在 M6；CI 在 M6 推迟、待仓库托管确定 |
| bench 基线数值归档（`bench/baselines/p0_utilities.json`） | **M7** | M5 建 bench 骨架，基线在 M7 定稿归档 |
| `engine`/`io`/`index` 等业务模块测试 | 各自阶段（P1+） | M5 只覆盖 P0 的 util/common |
| `CMakePresets.json` | M6（可选） | 与 `run-tests.sh` 一并 |

---

## 2. 现状盘点（读码结论）

- **M1 已留接入缝**：根 `CMakeLists.txt` 有 `enable_testing()`、`option(CABE_BUILD_TESTS …)`/`CABE_BUILD_BENCH`、以及 `if(CABE_BUILD_TESTS AND EXISTS test/CMakeLists.txt) add_subdirectory(test)` 守卫。M5 只需创建 `test/`、`bench/` 子目录内容，根 CMake **无需改动**（除覆盖率选项）。
- **依赖已装**：`scripts/setup-dev.sh` 的 `REQUIRED_PKGS` 含 `gtest-devel`、`gmock-devel`、`google-benchmark-devel` → `find_package` 可命中系统库；`FetchContent` 仅作无系统库（如精简 CI）时兜底。
- **被测对象 + 已有 smoke 结论可固化**：
  | 模块 | 一路验证过、可固化的结论 |
  |---|---|
  | `util/crc32` | CRC32C("123456789")=`0xE3069283`；空输入边界 |
  | `util/hash` | `Hash("")`=`0x2D06800538D394C2`(官方基准)；N=8 分布卡方≈1.70 |
  | `common/structs` | `BlockId` 编解码、`sizeof/alignof`、`ValueMeta` 字段 |
  | `common/error_code` | 段位不重叠、memory 段取值 |
  | `common/logger` | 格式 `[LEVEL][tid][file:line] msg`、默认 WARN 过滤 |
- **可测性现状（关键障碍）**：
  - `crc32.cpp` 的 `SoftwareCRC32C` / `HardwareCRC32C_x86` 在**匿名 namespace**，测试无法分别调用 → 测不了「软/硬一致性」（ROADMAP 明列）。
  - `logger` 的 `Threshold()` **首用即缓存**，一个进程只有一个阈值 → 单进程测不了多级别过滤；且输出走 stderr，需捕获。

---

## 3. 待 owner 终审的决策

### 决策-1：GTest / benchmark 接入方式 = `find_package` 优先 + `FetchContent` 兜底

| 维度 | 内容 |
|---|---|
| 裁决 | 与 ROADMAP 一致：`find_package(GTest)`/`find_package(benchmark)` 优先（系统库，`setup-dev.sh` 已装）；未命中则 `FetchContent` 拉取**钉死 tag**（建议 GTest `v1.15.2`、benchmark `v1.9.x`） |
| 与 M4 vendored 的区别 | 测试/基准工具**不影响产品正确性与冻结**，故用系统库 + 兜底即可，不必 vendoring；与 hash（必须冻结）的取舍不同 |
| 状态 | 建议采纳（ROADMAP 指定；仅 FetchContent tag 待定） |

### 决策-2（核心）：为可测性给 crc32 / logger 开「测试钩子」

| 维度 | 内容 |
|---|---|
| 问题 | 测「crc32 软/硬一致性」「logger 多级别过滤」必须触达当前被封装的内部（§2） |
| 方案 | **crc32**：把两实现从匿名 namespace 提升到 `cabe::util::detail`（或单独 `crc32_internal.h`），对外 API `CRC32` 不变；测试 include 内部头比对软/硬。**logger**：增设测试可见的阈值注入（如 `cabe::log::detail::SetThresholdForTest(Level)`）+ 测试内重定向 stderr 捕获 |
| 取舍 | 放宽一点 M2/M3 的"内部不外露"，换取 ROADMAP 明列的覆盖项；钩子置于 `detail`/`testing` 子命名空间、注释标注"仅测试用"，把外溢降到最低 |
| 备选 | ① 测试 `#include "crc32.cpp"` 直接拿匿名符号（hacky、与构建冲突）；② logger 多级别用**子进程**（`fork`+`setenv`+`exec`）避免改 logger（更重、平台相关） |
| 状态 | **待终审**：是否接受为测试开 `detail` 钩子（推荐），还是走子进程/不测该项 |

### 决策-3：覆盖率工具与门槛

| 维度 | 内容 |
|---|---|
| 工具 | 双工具链双轨：GCC = `--coverage`(gcov) + `gcovr`；Clang = `-fprofile-instr-generate -fcoverage-mapping` + `llvm-cov` |
| 开关 | 新增 `option(CABE_COVERAGE OFF)`；仅在该选项开时加插桩 flags（避免污染常规/Release 构建）|
| target | `coverage`：构建测试 → `ctest` → 生成行覆盖报告；门槛 util+common ≥ 80%（ROADMAP） |
| 状态 | 锁定（ROADMAP 指定 ≥80%；工具按编译器选） |

---

## 4. 框架接入（CMake）

`test/CMakeLists.txt`（示意）：
```cmake
# GTest：系统库优先，兜底 FetchContent（钉死 tag）
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)
# 每个被测模块一个测试可执行（或合并），统一链接 cabe::util / cabe::common + GTest::gtest_main
function(cabe_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE cabe::util cabe::common GTest::gtest_main)
    gtest_discover_tests(${name})    # 自动注册进 ctest
endfunction()

cabe_add_test(test_crc32        util/crc32_test.cpp)
cabe_add_test(test_hash         util/hash_test.cpp)
cabe_add_test(test_util         util/util_test.cpp)
cabe_add_test(test_cpu_features util/cpu_features_test.cpp)
cabe_add_test(test_structs      common/structs_test.cpp)
cabe_add_test(test_error_code   common/error_code_test.cpp)
cabe_add_test(test_logger       common/logger_test.cpp)
```

`bench/CMakeLists.txt`（示意）：
```cmake
find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
    include(FetchContent)
    FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.1)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
endif()
add_executable(bench_crc32 util/crc32_bench.cpp)
target_link_libraries(bench_crc32 PRIVATE cabe::util benchmark::benchmark_main)
add_executable(bench_hash util/hash_bench.cpp)
target_link_libraries(bench_hash PRIVATE cabe::util benchmark::benchmark_main)
```

- 根 `CMakeLists.txt` 的守卫（M1）已能在 `CABE_BUILD_TESTS=ON` 且 `test/CMakeLists.txt` 存在时纳入；M5 不改根 CMake 的该段。
- 默认 `CABE_BUILD_TESTS=OFF`（M1 决策）；本地/CI 显式 `-DCABE_BUILD_TESTS=ON`。

---

## 5. 目录结构

```
test/
  CMakeLists.txt
  util/   crc32_test.cpp  hash_test.cpp  util_test.cpp  cpu_features_test.cpp
  common/ structs_test.cpp  error_code_test.cpp  logger_test.cpp
bench/
  CMakeLists.txt
  util/   crc32_bench.cpp  hash_bench.cpp
  baselines/   (空，M7 归档 p0_utilities.json)
```

---

## 6. 测试用例清单（按 ROADMAP 覆盖目标）

| 文件 | 用例要点 |
|---|---|
| `util/crc32_test` | 已知向量 `CRC32C("123456789")==0xE3069283`；空 buffer（`size()==0`）；**软/硬一致性**：随机 buffer 上 `detail::Software==detail::Hardware`（仅当 `cpu::HasSSE42()`，否则跳过硬件，见 §7） |
| `util/hash_test` | 冻结基线 `Hash("")==0x2D06800538D394C2` 等若干钉死值；`Hash(DataView)==Hash(string_view)` 一致；100K 随机 key 分布卡方 < 临界；`RouteToDevice` 值域 `[0,N)` |
| `util/util_test` | `GetMonotonicTimeNs` 单调不减；`GetWallTimeNs`/`GetMonotonicTimeNs` 语义（两次调用差为正、量级合理） |
| `util/cpu_features_test` | smoke：`GetArch()` 返回非崩溃值；`HasSSE42/HasAVX2/HasARMCRC` 可调用且自洽（x86 上多次调用一致） |
| `common/structs_test` | `BlockId::Make(d,i)` 往返 `dev()==d`/`block_idx()==i`/`byte_offset()==i*kValueSize`（安全区 i）；`sizeof(BlockId)==8`、`sizeof(ValueMeta)==24`、`alignof==8`（运行时再确认）；`ValueMeta{}` 全零、`reserved` 为 0；`ValueState` 取值；`<=>` 比较 |
| `common/error_code_test` | 段基址数值、相邻段不重叠（运行时验证 `static_assert` 同款关系）；memory 段取值 `kMemNullPointer==-100000`… |
| `common/logger_test` | 捕获 stderr 验格式 `^\[WARN\]\[\d+\]\[logger_test.cpp:\d+\] …$`；**级别过滤**：注入阈值后 DEBUG 被过滤、ERROR 输出（依赖 §7 钩子）；非变参与变参两种调用 |

> 用例把"现状盘点"里所有 smoke 结论变成可回归的断言，尤其 `Hash("")` 与 CRC32C 向量是**冻结守护**（防 M4 vendored 升级 / crc32 改动悄悄改变输出）。

---

## 7. 可测性改造（§3 决策-2 的落地细节）

### 7.1 crc32 软/硬一致性
- 现状：两实现在匿名 namespace，外部不可见。
- 改造：提升到 `namespace cabe::util::detail`，并在 `crc32.h`（或新增 `crc32_internal.h`）声明：
  ```cpp
  namespace cabe::util::detail {
      std::uint32_t SoftwareCRC32C(DataView) noexcept;
  #if defined(__x86_64__) || defined(__i386__)
      std::uint32_t HardwareCRC32C_x86(DataView) noexcept;  // 仅 SSE4.2 可用时调用
  #endif
  }
  ```
  对外 `CRC32(DataView)` 接口与运行时分派**不变**。
- 测试：`HasSSE42()` 为真时，随机 buffer 上断言 `Software(buf)==Hardware(buf)==CRC32(buf)`；为假时只验软件路径（避免在无 SSE4.2 CPU 上执行硬件指令导致 SIGILL）。

### 7.2 logger 级别过滤 + stderr 捕获
- 级别：增设 `cabe::log::detail::SetThresholdForTest(Level)`（仅测试用，注释标注），让单进程内可切换阈值；或保留 `Threshold()` 首用缓存语义、由该钩子覆盖缓存值。
- 捕获：测试内 `freopen`/`dup2` 把 `STDERR_FILENO` 重定向到临时文件 → 调 `CABE_LOG_*` → 读回比对格式与是否被过滤；测试后恢复。
- 这样在单进程内即可覆盖「DEBUG 被默认 WARN 过滤」「设 DEBUG 后全部输出」「格式正确」。

> 两处钩子都置于 `detail` 子命名空间、明确"测试专用"，不进入对外 API 面；是否接受见 §3 决策-2。

---

## 8. 覆盖率

- `option(CABE_COVERAGE OFF)`；开启时对 `cabe_util`/`cabe_common`/测试加插桩：
  - GCC：`--coverage`（=`-fprofile-arcs -ftest-coverage`）；报告用 `gcovr -r . --html`。
  - Clang：`-fprofile-instr-generate -fcoverage-mapping`；报告用 `llvm-profdata merge` + `llvm-cov report`。
- `coverage` target：`cmake --build` → `ctest` → 生成报告 → 打印 util+common 行覆盖率。
- **门槛 ≥ 80%**（ROADMAP）。logger/error_code/structs 多为头内 `inline`/`constexpr`，覆盖率统计需确保被测 TU 实例化到（用例覆盖各分支）。

---

## 9. 关键设计决策

| # | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| M5-D1 | GTest/benchmark：`find_package` 优先 + `FetchContent` 兜底（钉死 tag） | 仅系统库 / 仅 FetchContent / vendoring | 与 ROADMAP 一致；测试工具不需冻结，无需 vendoring | 建议采纳 |
| M5-D2 | 为 crc32/logger 开 `detail` 测试钩子 | `#include .cpp` / 子进程 / 不测 | 触达被封装内部以覆盖 ROADMAP 明列项；钩子限 `detail`、外溢最小 | **待终审**（§3 决策-2） |
| M5-D3 | 覆盖率双轨（gcov/llvm-cov）+ `CABE_COVERAGE` 选项 + ≥80% | 单工具链 / 不设门槛 | 双工具链对称；选项隔离插桩；ROADMAP 定 80% | 锁定 |
| M5-D4 | 每模块一个测试可执行 + `gtest_discover_tests` | 单一大可执行 | 隔离、并行、定位清晰；`ctest` 自动注册 | 锁定 |
| M5-D5 | 冻结值（`Hash("")`、CRC32C 向量）写成断言常量 | 不固化 | 守护 D6 冻结 / 防 vendored 升级悄改输出 | 锁定 |
| M5-D6 | `test/`、`bench/` 分 `util`/`common` 子目录 | 平铺 | 与 ROADMAP 目录约定一致 | 锁定 |

---

## 10. 与 ROADMAP M5 一致性核对

| ROADMAP M5 要求 | 本设计 | 状态 |
|---|---|---|
| GTest+benchmark 接入（find_package / FetchContent） | §4 / M5-D1 | ✅ |
| `test/util` `test/common` `bench/util` | §5 | ✅ |
| crc32：已知向量 / 软硬一致 / 空边界 | §6 / §7.1 | ✅（软硬一致依赖 M5-D2） |
| hash：已知向量 / 分布 / 跨平台稳定 | §6（冻结基线 + 卡方） | ✅ |
| util：时间戳单调 / 语义 | §6 | ✅ |
| cpu_features：smoke | §6 | ✅ |
| structs：BlockId 往返 / ValueMeta 对齐 / enum | §6 | ✅ |
| error_code：段位不重叠 | §6 | ✅ |
| logger：级别过滤 / 格式 | §6 / §7.2 | ✅（级别过滤依赖 M5-D2） |
| 覆盖率工具 + `make coverage` + ≥80% | §8 | ✅ |
| 退出：`ctest` 全绿 + util/common ≥80% | §13 | ✅ |

---

## 11. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 测试钩子外溢 | `detail` 暴露内部，可能被业务误用 | 限 `detail`/`testing` 子命名空间 + 注释"仅测试"；review 把关 |
| logger 单进程多级别 | 首用缓存使阈值难切 | `SetThresholdForTest` 覆盖缓存（M5-D2）；否则退化为子进程方案 |
| 覆盖率统计头内 inline | 头内函数覆盖归属/统计差异 | 用例覆盖各分支；`gcovr`/`llvm-cov` 按 TU 汇总 |
| FetchContent 联网 | 无系统库时拉取需网络 | 优先 `find_package`（setup-dev 已装）；CI 用容器预装 |
| 软硬一致性在无 SSE4.2 机器 | 直接调硬件实现会 SIGILL | 测试前 `HasSSE42()` 守卫（§7.1） |

---

## 12. 退出条件（DoD）与验证步骤

1. `-DCABE_BUILD_TESTS=ON` 下，GCC 15 / Clang 20 双工具链构建出全部测试可执行；`ctest` **全绿**。
2. `-DCABE_BUILD_BENCH=ON` 下，bench 可执行可构建并运行（数值不作门槛，归档在 M7）。
3. `-DCABE_COVERAGE=ON` + `coverage` target：util+common **行覆盖率 ≥ 80%**。
4. 冻结守护用例通过：`CRC32C("123456789")==0xE3069283`、`Hash("")==0x2D06800538D394C2`。
5. 软/硬 CRC 一致性（SSE4.2 机器）、logger 级别过滤/格式 —— 通过（依赖 M5-D2 终审）。

---

## 13. 对下游里程碑的接口承诺

| 里程碑 | M5 提供的接入点 |
|---|---|
| M6 | 现成的 `ctest` 测试集，供本地八格组合矩阵直接复用；`run-tests.sh` / `run-coverage.sh` 包装（CI 推迟） |
| M7 | `bench/util` 骨架就绪，基线数值归档 `bench/baselines/p0_utilities.json` |
| P1+ | `test/<module>/` 目录约定 + `cabe_add_test` 模式 + 覆盖率 target，业务模块照搬 |
