# P5M2：WAL 核心（帧格式 + Wal 类 + 级别 1）。
# 设计依据：doc/P5/P5M2_wal_core_design.md
add_library(cabe_wal STATIC wal.cpp)
target_link_libraries(cabe_wal PUBLIC cabe_util)   # 用 RawDevice + CRC32；含 cabe_common（根 include）传递
add_library(cabe::wal ALIAS cabe_wal)
