# cabe_util：CRC32C / CPU 特性检测的静态库实现。
# 设计依据：doc/P0/P0M1_skeleton_design.md §5.4 / §6.9
add_library(cabe_util STATIC
    crc32.cpp
    cpu_features.cpp
    hash.cpp          # M4：xxh3 路由 hash（XXH_INLINE_ALL 内嵌 third_party/xxhash）
    raw_device.cpp    # P5M1：通用裸设备 I/O 工具
)
target_link_libraries(cabe_util PUBLIC cabe_common)   # 含 cabe_flags 传递
add_library(cabe::util ALIAS cabe_util)
