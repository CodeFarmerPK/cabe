# P0-M5：微基准。google-benchmark 系统库优先，未命中则 FetchContent 钉死 tag 兜底。
# 设计依据：doc/P0/P0M5_test_bench_design.md §4。基线数值归档在 M7（bench/baselines/）。
find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
    include(FetchContent)
    FetchContent_Declare(benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.1)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
endif()

add_executable(bench_crc32 util/crc32_bench.cpp)
target_link_libraries(bench_crc32 PRIVATE cabe::util benchmark::benchmark_main)

add_executable(bench_hash util/hash_bench.cpp)
target_link_libraries(bench_hash PRIVATE cabe::util benchmark::benchmark_main)
