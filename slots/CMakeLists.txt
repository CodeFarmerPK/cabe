# P4.5-M1：块分配器抽象层。
# 设计依据：doc/P4.5/P4.5M1_block_allocator_design.md
add_library(cabe_slots STATIC ring/ring_block_allocator.cpp)
target_link_libraries(cabe_slots PUBLIC cabe_common)
add_library(cabe::slots ALIAS cabe_slots)
