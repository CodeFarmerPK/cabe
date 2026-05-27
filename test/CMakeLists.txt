# P0-M5：单元测试。GTest 系统库优先，未命中则 FetchContent 钉死 tag 兜底。
# 设计依据：doc/P0/P0M5_test_bench_design.md §4
# P0M7 评审 #13：加版本下限 1.14（与 FetchContent 钉死的 v1.15.2 保持下行兼容）。
find_package(GTest 1.14 QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# 每个被测模块一个测试可执行；统一链接 cabe::util + cabe::common + GTest::gtest_main。
# DISCOVERY_TIMEOUT 60：sanitizer 二进制启动慢（ASAN/TSAN 注入 + shadow memory 映射），
#   默认 5s 在慢容器 / 首跑无缓存下易超时；超时会让该 target 在 ctest 报 0 测试静默通过。
function(cabe_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE cabe::util cabe::common GTest::gtest_main)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
endfunction()

cabe_add_test(test_crc32        util/crc32_test.cpp)
cabe_add_test(test_hash         util/hash_test.cpp)
cabe_add_test(test_util         util/util_test.cpp)
cabe_add_test(test_cpu_features util/cpu_features_test.cpp)
cabe_add_test(test_structs      common/structs_test.cpp)
cabe_add_test(test_error_code   common/error_code_test.cpp)

# P1M1：Engine 测试（链接 cabe::engine，不走 cabe_add_test 的默认链接列表）
add_executable(test_engine engine/engine_test.cpp)
target_link_libraries(test_engine PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_engine DISCOVERY_TIMEOUT 60)

add_executable(test_status engine/status_test.cpp)
target_link_libraries(test_status PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_status DISCOVERY_TIMEOUT 60)

# P1M2：BufferPool 测试
add_executable(test_buffer_pool engine/buffer_pool_test.cpp)
target_link_libraries(test_buffer_pool PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_buffer_pool DISCOVERY_TIMEOUT 60)

# P1M3：FreeList 测试
add_executable(test_free_list engine/free_list_test.cpp)
target_link_libraries(test_free_list PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_free_list DISCOVERY_TIMEOUT 60)

# P3M1：SyncIoBackend 测试
add_executable(test_sync_io_backend io/sync_io_backend_test.cpp)
target_link_libraries(test_sync_io_backend PRIVATE cabe::io cabe::engine GTest::gtest_main)
gtest_discover_tests(test_sync_io_backend DISCOVERY_TIMEOUT 60)

# P3M2：MetaIndex 契约测试
add_executable(test_meta_index_contract index/meta_index_contract_test.cpp)
target_link_libraries(test_meta_index_contract PRIVATE cabe::index GTest::gtest_main)
gtest_discover_tests(test_meta_index_contract DISCOVERY_TIMEOUT 60)

# P4M1：IoUringIoBackend 测试（仅 io_uring 后端时编译）
if(CABE_IO_BACKEND STREQUAL "io_uring")
    add_executable(test_io_uring_backend io/io_uring_backend_test.cpp)
    target_link_libraries(test_io_uring_backend PRIVATE cabe::io cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_io_uring_backend DISCOVERY_TIMEOUT 60)
endif()
