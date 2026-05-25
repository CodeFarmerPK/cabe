#ifndef CABE_DEVICE_CONTEXT_H
#define CABE_DEVICE_CONTEXT_H

#include "engine/buffer_pool.h"
#include "engine/free_list.h"
#include "engine/meta_index.h"

// 内部类型，不在公开 API 承诺内。

namespace cabe {

    struct DeviceContext {
        int fd = -1;
        BufferPool pool{0};
        FreeList free_list;
        MetaIndex meta_index;
    };

} // namespace cabe

#endif // CABE_DEVICE_CONTEXT_H
