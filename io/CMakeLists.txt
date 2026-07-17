# P3-M1 + P4-M1 + P9M1：IoBackend 抽象层。
# 设计依据：doc/P3/P3M1_io_backend_design.md / doc/P4/P4M1_io_uring_basic_design.md /
#           doc/P9/P9M1_build_config_design.md
if(CABE_IO_BACKEND STREQUAL "spdk")
    add_library(cabe_io STATIC spdk/spdk_io_backend_placeholder.cpp)
else()
    add_library(cabe_io STATIC sync/sync_io_backend.cpp)
endif()
target_link_libraries(cabe_io PUBLIC cabe_common)

if(CABE_IO_BACKEND STREQUAL "io_uring")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.9)
    target_sources(cabe_io PRIVATE uring/io_uring_backend.cpp)
    target_link_libraries(cabe_io PUBLIC PkgConfig::LIBURING)
elseif(CABE_IO_BACKEND STREQUAL "spdk")
    # SPDK headers and link closure stay private to the backend boundary.
    target_link_libraries(cabe_io PRIVATE cabe::spdk_deps)

    add_executable(cabe_spdk_link_check spdk/spdk_link_check.cpp)
    target_link_libraries(cabe_spdk_link_check PRIVATE cabe::common cabe::spdk_deps)
    if(CABE_BUILD_TESTS)
        add_test(NAME SpdkLinkCheck COMMAND cabe_spdk_link_check)
        set_tests_properties(SpdkLinkCheck PROPERTIES LABELS "spdk;spdk-link")
    endif()
endif()

add_library(cabe::io ALIAS cabe_io)
