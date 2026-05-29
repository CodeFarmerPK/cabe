# P4.5-M2：Engine 切换到块分配器抽象层。
# 设计依据：doc/P4.5/P4.5M2_engine_switch_design.md
add_library(cabe_engine STATIC
    engine.cpp
    buffer_pool.cpp
    super_block.cpp
)
target_link_libraries(cabe_engine PUBLIC cabe_util cabe_io cabe_index cabe_slots)
add_library(cabe::engine ALIAS cabe_engine)

# ---- IoBackend 编译期分派 ----
if(CABE_IO_BACKEND STREQUAL "sync")
    target_compile_definitions(cabe_engine PUBLIC CABE_USE_IO_SYNC=1)
elseif(CABE_IO_BACKEND STREQUAL "io_uring")
    target_compile_definitions(cabe_engine PUBLIC CABE_USE_IO_URING=1)
else()
    message(FATAL_ERROR "CABE_IO_BACKEND='${CABE_IO_BACKEND}' 尚未实现")
endif()

# ---- MetaIndex 编译期分派 ----
if(CABE_META_INDEX STREQUAL "hashmap")
    target_compile_definitions(cabe_engine PUBLIC CABE_USE_META_HASHMAP=1)
else()
    message(FATAL_ERROR "CABE_META_INDEX='${CABE_META_INDEX}' 尚未实现")
endif()

# ---- BlockAllocator 编译期分派 ----
if(CABE_BLOCK_ALLOCATOR STREQUAL "ring_queue")
    target_compile_definitions(cabe_engine PUBLIC CABE_USE_BLOCK_RING_QUEUE=1)
else()
    message(FATAL_ERROR "CABE_BLOCK_ALLOCATOR='${CABE_BLOCK_ALLOCATOR}' 尚未实现")
endif()
