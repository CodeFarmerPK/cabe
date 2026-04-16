cmake_minimum_required(VERSION 3.14)
project(cabe)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
        buffer/buffer_pool.cpp
        test/buffer/buffer_pool_test.cpp
)

# 静态库，供 main 和 test 共用
add_library(cabe_lib STATIC ${CABE_LIB_SOURCES})
target_include_directories(cabe_lib PUBLIC ${CMAKE_SOURCE_DIR})

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
    )

    add_executable(cabe_test ${TEST_SOURCES})
    target_link_libraries(cabe_test PRIVATE cabe_lib GTest::gtest_main)
    target_include_directories(cabe_test PRIVATE ${CMAKE_SOURCE_DIR})

    include(GoogleTest)
    gtest_discover_tests(cabe_test)
endif()