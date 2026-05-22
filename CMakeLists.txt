# ============================================================
# Cabe —— 定长 value 键值存储引擎（仅 Linux / Fedora 43+）
# 根 CMake：平台/工具链/标准三道配置期校验 + 预留选项 + 目标拓扑
# 设计依据：doc/P0/P0M1_skeleton_design.md
# ============================================================
cmake_minimum_required(VERSION 3.30)
project(cabe VERSION 0.0.1 LANGUAGES CXX)

# ---------- 平台校验（仅 Linux）----------
# 与 common/structs.h 的 #error 互补：配置期即拦截跨平台误配。
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Cabe only supports Linux (target: Fedora 43). See README.md.")
endif()

# ---------- 工具链校验（GCC >= 15 / Clang >= 20）----------
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

# ---------- C++ 标准 ----------
# EXTENSIONS OFF -> 用 -std=c++20 而非 gnu++20。现有 GNU 特性
# （[[gnu::target]] / <cpuid.h>）均不依赖语言扩展，故安全。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------- 默认构建类型 ----------
# 单配置生成器（默认 Ninja）未指定时落 Release，与 README 一致；多配置不强加。
get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

# ---------- 预留选项 ----------
# CABE_SANITIZER 在 M1 即应用编译开关（见设计 §3 偏差-1）。
set(CABE_SANITIZER "none" CACHE STRING "Sanitizer: none|address|thread|undefined")
set_property(CACHE CABE_SANITIZER PROPERTY STRINGS none address thread undefined)
# 以下两项 M1 仅声明 + 校验，真实编译期分派在 P3（IO）/ P3·P9（索引）。
set(CABE_IO_BACKEND "sync" CACHE STRING "I/O backend: sync|io_uring|spdk")
set_property(CACHE CABE_IO_BACKEND PROPERTY STRINGS sync io_uring spdk)
set(CABE_META_INDEX "hashmap" CACHE STRING "Meta index: hashmap|bplustree")
set_property(CACHE CABE_META_INDEX PROPERTY STRINGS hashmap bplustree)

# 构建开关（默认 OFF，M5 起置 ON）
option(CABE_BUILD_TESTS "Build unit tests"        OFF)
option(CABE_BUILD_BENCH "Build benchmarks"        OFF)
option(CABE_WERROR      "Treat warnings as errors" OFF)
option(CABE_COVERAGE    "Instrument util/common for line coverage (gcov / llvm-cov)" OFF)

# ---------- 选项合法值校验（非法值 fail-fast）----------
function(cabe_validate_choice var)
    set(_allowed ${ARGN})
    if(NOT "${${var}}" IN_LIST _allowed)
        message(FATAL_ERROR "Invalid ${var}='${${var}}'. Allowed: ${_allowed}")
    endif()
endfunction()

cabe_validate_choice(CABE_SANITIZER  none address thread undefined)
cabe_validate_choice(CABE_IO_BACKEND sync io_uring spdk)
cabe_validate_choice(CABE_META_INDEX hashmap bplustree)

# ---------- 非默认值提示（避免误以为已生效）----------
if(NOT CABE_IO_BACKEND STREQUAL "sync")
    message(STATUS "CABE_IO_BACKEND='${CABE_IO_BACKEND}' recorded; real dispatch lands in P3.")
endif()
if(NOT CABE_META_INDEX STREQUAL "hashmap")
    message(STATUS "CABE_META_INDEX='${CABE_META_INDEX}' recorded; real dispatch lands in P3/P9.")
endif()

# ---------- cabe_flags：统一编译契约（接口库）----------
add_library(cabe_flags INTERFACE)
target_compile_features(cabe_flags INTERFACE cxx_std_20)
target_compile_options(cabe_flags INTERFACE -Wall -Wextra -Wpedantic)
# 把"一旦触发基本等于未定义行为"的三条告警默认升级为硬错误（无论 CABE_WERROR 如何）：
#   return-type          非 void 函数漏 return  -> 调用处读到未定义值
#   uninitialized        读未初始化变量          -> 未定义值
#   implicit-fallthrough switch 漏 break 未标注  -> 逻辑 bug
# 仅这三条 default-on；全量 -Werror 仍经 CABE_WERROR（默认 OFF），避免编译器
# 升级带来的新增风格告警直接卡死本地/CI。用 INTERFACE target 施加，不污染第三方。
target_compile_options(cabe_flags INTERFACE
    -Werror=return-type
    -Werror=uninitialized
    -Werror=implicit-fallthrough)
if(CABE_WERROR)
    target_compile_options(cabe_flags INTERFACE -Werror)
endif()
if(NOT CABE_SANITIZER STREQUAL "none")
    # -g：sanitizer 报告需调试信息才能符号化到 源文件:行号。默认 Release 不含 -g，
    # 故显式补上；sanitizer 构建建议配 -DCMAKE_BUILD_TYPE=Debug 或 RelWithDebInfo。
    target_compile_options(cabe_flags INTERFACE -fsanitize=${CABE_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(cabe_flags INTERFACE -fsanitize=${CABE_SANITIZER})
    message(STATUS "Sanitizer enabled: ${CABE_SANITIZER} (use Debug/RelWithDebInfo for best stack traces)")
endif()

# ---------- 覆盖率插桩（M5；仅 CABE_COVERAGE=ON 时启用，避免污染常规/Release 构建）----------
if(CABE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(cabe_flags INTERFACE --coverage -O0 -g)
        target_link_options(cabe_flags INTERFACE --coverage)
    else() # Clang
        target_compile_options(cabe_flags INTERFACE -fprofile-instr-generate -fcoverage-mapping -O0 -g)
        target_link_options(cabe_flags INTERFACE -fprofile-instr-generate)
    endif()
    message(STATUS "Coverage instrumentation: ON (${CMAKE_CXX_COMPILER_ID}); 报告用 gcovr/llvm-cov，见 doc/P0/P0M5 §8")
endif()

# ---------- 子目录 ----------
enable_testing()                     # 便宜，提前就绪；具体 test 目标 M5 再加
add_subdirectory(common)
add_subdirectory(util)
# test/ bench/ 推迟到 M5（设计 §3 偏差-2）；EXISTS 守卫使 M1/M5 无需回改本文件。
if(CABE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    add_subdirectory(test)
endif()
if(CABE_BUILD_BENCH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt")
    add_subdirectory(bench)
endif()
