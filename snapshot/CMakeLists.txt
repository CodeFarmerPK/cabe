# P5M4：快照模块（检查点）。与 wal/、io/ 平级——三设备三管理模块。
# 设计依据：doc/P5/P5M4_snapshot_design.md
add_library(cabe_snapshot STATIC snapshot.cpp)
# 用 RawDevice + CRC32（cabe_util）；含 cabe_common（根 include）传递，故 engine/super_block.h、
# index/meta_index.h 等头文件可见。不链 cabe_engine/cabe_index——只用其头文件，避免与 engine 循环依赖。
target_link_libraries(cabe_snapshot PUBLIC cabe_util)
add_library(cabe::snapshot ALIAS cabe_snapshot)
