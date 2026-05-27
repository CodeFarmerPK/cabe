# P3-M1 + P4-M1：IoBackend 抽象层。
# 设计依据：doc/P3/P3M1_io_backend_design.md / doc/P4/P4M1_io_uring_basic_design.md
add_library(cabe_io STATIC sync/sync_io_backend.cpp)
target_link_libraries(cabe_io PUBLIC cabe_common)

if(CABE_IO_BACKEND STREQUAL "io_uring")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.9)
    target_sources(cabe_io PRIVATE uring/io_uring_backend.cpp)
    target_link_libraries(cabe_io PUBLIC PkgConfig::LIBURING)
endif()

add_library(cabe::io ALIAS cabe_io)
