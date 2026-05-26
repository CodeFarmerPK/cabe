# P3-M2：MetaIndex 抽象层。
# 设计依据：doc/P3/P3M2_meta_index_design.md
add_library(cabe_index STATIC hash/hash_meta_index.cpp)
target_link_libraries(cabe_index PUBLIC cabe_common)
add_library(cabe::index ALIAS cabe_index)
