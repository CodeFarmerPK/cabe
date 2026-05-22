# cabe_common：纯头库 + 单一 include 根（= 工程根）。
# 设计依据：doc/P0/P0M1_skeleton_design.md §5.3 / §6.9
add_library(cabe_common INTERFACE)
target_include_directories(cabe_common INTERFACE ${PROJECT_SOURCE_DIR})
target_link_libraries(cabe_common INTERFACE cabe_flags)
add_library(cabe::common ALIAS cabe_common)
