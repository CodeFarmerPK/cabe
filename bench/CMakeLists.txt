# P0-M5：微基准。google-benchmark 系统库优先，未命中则 FetchContent 钉死 tag 兜底。
# 设计依据：doc/P0/P0M5_test_bench_design.md §4。基线数值归档在 M7（bench/baselines/）。
# P0M7 评审 #13：加版本下限 1.8（与 FetchContent 钉死的 v1.9.1 保持下行兼容）。
find_package(benchmark 1.8 QUIET)
if(NOT benchmark_FOUND)
    include(FetchContent)
    FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.1)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
endif()

# P0M7 评审 #12：显式链接 cabe::common，不依赖 cabe::util PUBLIC 传递；
# 即便日后 util 把 common 改 PRIVATE，bench 目标也不至于编译失败。
add_executable(bench_crc32 util/crc32_bench.cpp)
target_link_libraries(bench_crc32 PRIVATE cabe::util cabe::common benchmark::benchmark_main)

add_executable(bench_hash util/hash_bench.cpp)
target_link_libraries(bench_hash  PRIVATE cabe::util cabe::common benchmark::benchmark_main)

# P1M5：Engine 微基准（Put / Get / Delete 吞吐）
add_executable(bench_engine engine/engine_bench.cpp)
target_link_libraries(bench_engine PRIVATE cabe::engine benchmark::benchmark_main)
