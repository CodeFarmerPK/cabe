# P1-M1：引擎骨架。
# 设计依据：doc/P1/P1M1_engine_skeleton_design.md
add_library(cabe_engine STATIC engine.cpp)
target_link_libraries(cabe_engine PUBLIC cabe_util)
add_library(cabe::engine ALIAS cabe_engine)
