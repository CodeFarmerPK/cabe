# P3-M1：IoBackend 抽象层。
# 设计依据：doc/P3/P3M1_io_backend_design.md
add_library(cabe_io STATIC sync/sync_io_backend.cpp)
target_link_libraries(cabe_io PUBLIC cabe_common)
add_library(cabe::io ALIAS cabe_io)
