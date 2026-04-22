cmake_minimum_required(VERSION 3.14)
project(cabe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# 平台与工具链校验 —— Cabe 仅支持 Fedora 43
# 详见 README.md "Supported Platform" 章节
# ============================================================

# --- Linux 硬性要求（O_DIRECT / mmap / pread / pwrite / io_uring）---
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "Cabe only supports Linux (target: Fedora 43). Detected: ${CMAKE_SYSTEM_NAME}.")
endif()

# --- 发行版：仅 Fedora 43 ---
if(NOT EXISTS /etc/os-release)
    message(FATAL_ERROR "Cannot detect distribution (/etc/os-release missing). Cabe requires Fedora 43.")
endif()
file(READ /etc/os-release CABE_OS_RELEASE)
string(REGEX MATCH "\nID=([^\n]+)"         _ "\n${CABE_OS_RELEASE}")
set(CABE_DISTRO_ID "${CMAKE_MATCH_1}")
string(REGEX MATCH "\nVERSION_ID=([^\n]+)" _ "\n${CABE_OS_RELEASE}")
set(CABE_DISTRO_VERSION "${CMAKE_MATCH_1}")
string(STRIP "${CABE_DISTRO_ID}"      CABE_DISTRO_ID)
string(STRIP "${CABE_DISTRO_VERSION}" CABE_DISTRO_VERSION)

if(NOT CABE_DISTRO_ID STREQUAL "fedora")
    message(FATAL_ERROR
        "Cabe only supports Fedora 43. Detected: ${CABE_DISTRO_ID} ${CABE_DISTRO_VERSION}.")
endif()
if(CABE_DISTRO_VERSION VERSION_LESS 43)
    message(FATAL_ERROR
        "Cabe requires Fedora 43+. Detected: Fedora ${CABE_DISTRO_VERSION}.")
endif()
message(STATUS "Distribution: Fedora ${CABE_DISTRO_VERSION}")

# --- 编译器：GCC 15+ 或 Clang 20+（Fedora 43 默认工具链）---
if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
    message(FATAL_ERROR "Cabe requires GCC or Clang. Detected: ${CMAKE_CXX_COMPILER_ID}")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15.0)
    message(FATAL_ERROR
        "Cabe requires GCC 15+ (Fedora 43 ships GCC 15). Detected: ${CMAKE_CXX_COMPILER_VERSION}.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20.0)
    message(FATAL_ERROR
        "Cabe requires Clang 20+ (Fedora 43 ships Clang 20). Detected: ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

# --- 内核：Fedora 43 发布内核 6.16+（软校验）---
execute_process(
    COMMAND uname -r
    OUTPUT_VARIABLE CABE_KERNEL_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "^([0-9]+\\.[0-9]+)" _ "${CABE_KERNEL_VERSION}")
set(CABE_KERNEL_SHORT "${CMAKE_MATCH_1}")
if(CABE_KERNEL_SHORT VERSION_LESS 6.16)
    message(WARNING
        "Cabe targets kernel 6.16+ (Fedora 43). Detected: ${CABE_KERNEL_VERSION}. "
        "Newer io_uring features (P3+) may be unavailable.")
endif()

# --- 必需的系统头文件 ---
include(CheckIncludeFileCXX)
check_include_file_cxx(sys/mman.h CABE_HAVE_SYS_MMAN_H)
check_include_file_cxx(fcntl.h    CABE_HAVE_FCNTL_H)
check_include_file_cxx(unistd.h   CABE_HAVE_UNISTD_H)
if(NOT CABE_HAVE_SYS_MMAN_H OR NOT CABE_HAVE_FCNTL_H OR NOT CABE_HAVE_UNISTD_H)
    message(FATAL_ERROR "Missing required POSIX headers (sys/mman.h / fcntl.h / unistd.h)")
endif()

message(STATUS "Kernel:   ${CABE_KERNEL_VERSION}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# ============================================================
# Sanitizer 选项（CI 会在 ASAN/TSAN/UBSAN 矩阵里各跑一遍）
# ============================================================
option(CABE_ENABLE_TSAN  "Enable ThreadSanitizer"            OFF)
option(CABE_ENABLE_ASAN  "Enable AddressSanitizer"           OFF)
option(CABE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(CABE_ENABLE_TSAN AND CABE_ENABLE_ASAN)
    message(FATAL_ERROR "TSAN and ASAN cannot be enabled simultaneously")
endif()
if(CABE_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
    message(STATUS "ThreadSanitizer: ON")
endif()
if(CABE_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
    message(STATUS "AddressSanitizer: ON")
endif()
if(CABE_ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined)
    add_link_options(-fsanitize=undefined)
    message(STATUS "UBSanitizer: ON")
endif()

# 通用警告
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# 把"一旦触发基本等于 UB"的几条警告升级为硬错误。
# 这些不是"风格警告"，而是"代码写错了"的信号：
#   -Werror=return-type        非 void 函数漏 return → 调用处读到未定义值
#   -Werror=uninitialized      读未初始化变量 → 未定义值
#   -Werror=implicit-fallthrough switch fallthrough 未标注 → 逻辑 bug
# 不全开 -Werror，避免 third-party header 的警告拖垮构建
add_compile_options(
        -Werror=return-type
        -Werror=uninitialized
        -Werror=implicit-fallthrough
)

# ============================================================
# 项目源文件（不含 main.cpp）
# ============================================================
set(CABE_LIB_SOURCES
        util/crc32.cpp
        memory/meta_index.cpp
        memory/chunk_index.cpp
        storage/free_list.cpp
        storage/storage.cpp
        engine/engine.cpp
        engine/engine_api.cpp
        buffer/buffer_pool.cpp
        util/cpu_features.cpp
)

# 静态库，供 main 和 test 共用
add_library(cabe_lib STATIC ${CABE_LIB_SOURCES})
# Pimpl 隔离：
#   PUBLIC  ${CMAKE_SOURCE_DIR}/include  → 仅暴露 cabe/* 公开头给下游 link
#   PRIVATE ${CMAKE_SOURCE_DIR}          → 内部头(buffer/, memory/, storage/, engine/)
#                                          仅 cabe_lib 自身可见，下游绕过 Pimpl 会编译失败
# cabe_test / cabe_bench 显式声明 PRIVATE 内部头权限，不受影响。
target_include_directories(cabe_lib
        PUBLIC  ${CMAKE_SOURCE_DIR}/include
        PRIVATE ${CMAKE_SOURCE_DIR}
)

# ============================================================
# 主可执行文件
# ============================================================
add_executable(cabe main.cpp)
target_link_libraries(cabe PRIVATE cabe_lib)

# ============================================================
# 单元测试（gtest）
# ============================================================
option(CABE_BUILD_TESTS "Build unit tests" ON)

if(CABE_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
                googletest
                GIT_REPOSITORY https://github.com/google/googletest.git
                GIT_TAG        v1.14.0
        )
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()

    set(TEST_SOURCES
            test/memory/meta_index_test.cpp
            test/memory/chunk_index_test.cpp
            test/buffer/buffer_pool_test.cpp
            test/storage/free_list_test.cpp
            test/util/crc32_test.cpp
            test/engine/engine_test.cpp
            test/util/cpu_features_test.cpp
            test/engine/engine_thread_test.cpp
            test/engine/engine_api_test.cpp
            test/engine/cabe_engine_thread_test.cpp
    )

    add_executable(cabe_test ${TEST_SOURCES})
    target_link_libraries(cabe_test PRIVATE cabe_lib GTest::gtest_main)
    # cabe_lib 已将 include/ 通过 PUBLIC 暴露，cabe_test 通过 transitive 依赖自动继承；
    # 此处显式补充保持声明清晰。
    target_include_directories(cabe_test PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/include
    )
    include(GoogleTest)
    gtest_discover_tests(cabe_test)
endif()


# ============================================================
# 基准测试（google-benchmark）
# ============================================================
option(CABE_BUILD_BENCH "Build performance benchmarks" OFF)

if(CABE_BUILD_BENCH)
    find_package(benchmark QUIET)
    if(NOT benchmark_FOUND)
        include(FetchContent)
        set(BENCHMARK_ENABLE_TESTING  OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
                googlebenchmark
                GIT_REPOSITORY https://github.com/google/benchmark.git
                GIT_TAG        v1.8.3
        )
        FetchContent_MakeAvailable(googlebenchmark)
    endif()

    set(BENCH_SOURCES
            bench/buffer_pool_bench.cpp
            bench/meta_index_bench.cpp
            bench/chunk_index_bench.cpp
            bench/free_list_bench.cpp
            bench/crc32_bench.cpp
            bench/engine_bench.cpp
            bench/cabe_engine_bench.cpp
    )

    add_executable(cabe_bench ${BENCH_SOURCES})
    target_link_libraries(cabe_bench
            PRIVATE cabe_lib benchmark::benchmark_main)
    target_include_directories(cabe_bench
            PRIVATE ${CMAKE_SOURCE_DIR})
endif()