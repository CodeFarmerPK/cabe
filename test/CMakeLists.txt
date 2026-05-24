# P0-M5：单元测试。GTest 系统库优先，未命中则 FetchContent 钉死 tag 兜底。
# 设计依据：doc/P0/P0M5_test_bench_design.md §4
find_package(GTest QUIET)
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
