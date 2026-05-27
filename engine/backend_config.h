#ifndef CABE_BACKEND_CONFIG_H
#define CABE_BACKEND_CONFIG_H

// P3-M3：编译期后端分派。
// CMake 根据 CABE_IO_BACKEND / CABE_META_INDEX 传递编译定义，
// 本头文件据此选择具体实现类型。
// 设计依据：doc/P3/P3M3_engine_switch_design.md §3

// ---- IoBackend 分派 ----
#if defined(CABE_USE_IO_SYNC)
#include "io/sync/sync_io_backend.h"
namespace cabe { using IoBackendImpl = SyncIoBackend; }
#elif defined(CABE_USE_IO_URING)
#include "io/uring/io_uring_backend.h"
namespace cabe { using IoBackendImpl = IoUringIoBackend; }
#else
#error "未选择 IoBackend：请设置 -DCABE_IO_BACKEND=sync 或 io_uring"
#endif

// ---- MetaIndex 分派 ----
#if defined(CABE_USE_META_HASHMAP)
#include "index/hash/hash_meta_index.h"
namespace cabe { using MetaIndexImpl = HashMetaIndex; }
#else
#error "未选择 MetaIndex：请设置 -DCABE_META_INDEX=hashmap"
#endif

#include "io/io_backend.h"
#include "index/meta_index.h"
static_assert(cabe::IoBackend<cabe::IoBackendImpl>);
static_assert(cabe::MetaIndexBackend<cabe::MetaIndexImpl>);

#endif // CABE_BACKEND_CONFIG_H
