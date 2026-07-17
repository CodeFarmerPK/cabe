# P0-M5 + P9M1：单元测试。GTest 系统库优先，未命中则 FetchContent 钉死 tag 兜底。
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

# DISCOVERY_TIMEOUT 60：sanitizer 二进制启动较慢，避免发现阶段静默得到 0 项测试。
function(cabe_add_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE cabe::util cabe::common GTest::gtest_main)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
endfunction()

# ---- 所有后端都应继续运行的单元测试 ----
cabe_add_test(test_crc32        util/crc32_test.cpp)
cabe_add_test(test_hash         util/hash_test.cpp)
cabe_add_test(test_util         util/util_test.cpp)
cabe_add_test(test_cpu_features util/cpu_features_test.cpp)
cabe_add_test(test_structs      common/structs_test.cpp)
cabe_add_test(test_error_code   common/error_code_test.cpp)

add_executable(test_status engine/status_test.cpp)
target_link_libraries(test_status PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_status DISCOVERY_TIMEOUT 60)

add_executable(test_buffer_pool engine/buffer_pool_test.cpp)
target_link_libraries(test_buffer_pool PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_buffer_pool DISCOVERY_TIMEOUT 60)

add_executable(test_value_buffer_pool engine/value_buffer_pool_test.cpp)
target_link_libraries(test_value_buffer_pool PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_value_buffer_pool DISCOVERY_TIMEOUT 60)

add_executable(test_value_buffer_api engine/value_buffer_api_test.cpp)
target_link_libraries(test_value_buffer_api PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_value_buffer_api DISCOVERY_TIMEOUT 60)

add_executable(test_put_path engine/put_path_test.cpp)
target_link_libraries(test_put_path PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_put_path DISCOVERY_TIMEOUT 60)

add_executable(test_io_write_buffer io/io_write_buffer_test.cpp)
target_link_libraries(test_io_write_buffer PRIVATE cabe::io GTest::gtest_main)
gtest_discover_tests(test_io_write_buffer DISCOVERY_TIMEOUT 60)

add_executable(test_meta_index_contract index/meta_index_contract_test.cpp)
target_link_libraries(test_meta_index_contract PRIVATE cabe::index GTest::gtest_main)
gtest_discover_tests(test_meta_index_contract DISCOVERY_TIMEOUT 60)

add_executable(test_block_allocator_contract slots/block_allocator_contract_test.cpp)
target_link_libraries(test_block_allocator_contract PRIVATE cabe::slots GTest::gtest_main)
gtest_discover_tests(test_block_allocator_contract DISCOVERY_TIMEOUT 60)

if(CABE_IO_BACKEND STREQUAL "spdk")
    # M1 的 SPDK Engine 尚不可打开。这里只注册配置、测试门禁和真实链接证据，
    # 不把 Raw Engine 测试用 skip 包装成 SPDK 覆盖。
    add_executable(test_spdk_config engine/spdk_config_test.cpp)
    target_link_libraries(test_spdk_config PRIVATE cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_spdk_config DISCOVERY_TIMEOUT 60
        PROPERTIES LABELS "spdk\\;spdk-unit")

    add_executable(test_spdk_test_env
        common/spdk_test_env_test.cpp
        common/spdk_test_env.cpp)
    target_link_libraries(test_spdk_test_env PRIVATE cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_spdk_test_env DISCOVERY_TIMEOUT 60
        PROPERTIES LABELS "spdk\\;spdk-unit")
else()
    # ---- Raw 设备及可成功 Open 的现有 Engine 测试 ----
    cabe_add_test(test_raw_device util/raw_device_test.cpp)

    add_executable(test_engine engine/engine_test.cpp)
    target_link_libraries(test_engine PRIVATE cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_engine DISCOVERY_TIMEOUT 60)

    add_executable(test_value_buffer engine/value_buffer_test.cpp)
    target_link_libraries(test_value_buffer PRIVATE cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_value_buffer DISCOVERY_TIMEOUT 60)

    add_executable(test_sync_io_backend io/sync_io_backend_test.cpp)
    target_link_libraries(test_sync_io_backend PRIVATE cabe::io cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_sync_io_backend DISCOVERY_TIMEOUT 60)

    add_executable(test_super_block engine/super_block_test.cpp)
    target_link_libraries(test_super_block PRIVATE cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_super_block DISCOVERY_TIMEOUT 60)

    add_executable(test_wal wal/wal_test.cpp)
    target_link_libraries(test_wal PRIVATE cabe::engine cabe::wal GTest::gtest_main)
    gtest_discover_tests(test_wal DISCOVERY_TIMEOUT 60)

    add_executable(test_wal_concurrency wal/wal_concurrency_test.cpp)
    target_link_libraries(test_wal_concurrency PRIVATE cabe::wal GTest::gtest_main)
    gtest_discover_tests(test_wal_concurrency DISCOVERY_TIMEOUT 60)

    add_executable(test_snapshot snapshot/snapshot_test.cpp)
    target_link_libraries(test_snapshot PRIVATE cabe::engine cabe::snapshot GTest::gtest_main)
    gtest_discover_tests(test_snapshot DISCOVERY_TIMEOUT 60)

    add_executable(test_recovery engine/recovery_test.cpp)
    target_link_libraries(test_recovery PRIVATE cabe::engine cabe::snapshot cabe::wal GTest::gtest_main)
    gtest_discover_tests(test_recovery DISCOVERY_TIMEOUT 60)

    add_executable(test_reactor engine/reactor_test.cpp)
    target_link_libraries(test_reactor PRIVATE cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_reactor DISCOVERY_TIMEOUT 60)

    add_executable(test_engine_concurrency engine/engine_concurrency_test.cpp)
    target_link_libraries(test_engine_concurrency PRIVATE cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_engine_concurrency DISCOVERY_TIMEOUT 60)

    add_executable(test_engine_multidevice engine/engine_multidevice_test.cpp)
    target_link_libraries(test_engine_multidevice PRIVATE cabe::engine GTest::gtest_main)
    gtest_discover_tests(test_engine_multidevice DISCOVERY_TIMEOUT 60)

    if(CABE_IO_BACKEND STREQUAL "io_uring")
        add_executable(test_io_uring_backend io/io_uring_backend_test.cpp)
        target_link_libraries(test_io_uring_backend PRIVATE cabe::io cabe::engine GTest::gtest_main)
        gtest_discover_tests(test_io_uring_backend DISCOVERY_TIMEOUT 60)
    endif()
endif()
