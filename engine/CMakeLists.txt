# P1-M1 骨架 + P1-M2 数据通路。
# 设计依据：doc/P1/P1M1_engine_skeleton_design.md / P1M2_data_path_design.md
add_library(cabe_engine STATIC
    engine.cpp
    buffer_pool.cpp
    io.cpp
    free_list.cpp
    meta_index.cpp
)
target_link_libraries(cabe_engine PUBLIC cabe_util)
add_library(cabe::engine ALIAS cabe_engine)
