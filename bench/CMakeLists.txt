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

# P1M5：Engine 微基准（Put / Get / Delete 吞吐，单设备单线程）
add_executable(bench_engine engine/engine_bench.cpp)
target_link_libraries(bench_engine PRIVATE cabe::engine benchmark::benchmark_main)

# P7：多设备 + 并发 engine 吞吐微基准（per-device reactor 无锁并发 + 多设备路由的扩展性；
#   Threads(1/2/4/8) 扫并发，需两组设备 CABE_TEST_*[ 与 *2]，缺则该 bench 自跳过）。
#   run-bench 自动发现 bench_* 运行，新增此目标不需改脚本。
add_executable(bench_engine_mt engine/engine_mt_bench.cpp)
target_link_libraries(bench_engine_mt PRIVATE cabe::engine benchmark::benchmark_main)

# P8M5：三类 value 来源的 Put 微基准（非对齐自备值内存 / 对齐自备值内存 / Cabe ValueBuffer）。
add_executable(bench_engine_value_put engine/engine_value_put_bench.cpp)
target_link_libraries(bench_engine_value_put PRIVATE cabe::engine benchmark::benchmark_main)

# P6M3：多线程 Wal 吞吐微基准（直打 Wal，测 group commit 并发收益）。
# 链 cabe::wal（与 test_wal_concurrency 同款，直打 Wal、不经 Engine）。
add_executable(bench_wal_concurrency wal/wal_concurrency_bench.cpp)
target_link_libraries(bench_wal_concurrency PRIVATE cabe::wal cabe::common benchmark::benchmark_main)
