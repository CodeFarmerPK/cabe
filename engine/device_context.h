#ifndef CABE_DEVICE_CONTEXT_H
#define CABE_DEVICE_CONTEXT_H

#include "engine/buffer_pool.h"

// 内部类型，不在公开 API 承诺内。
// P1M2 补入 BufferPool；后续 milestone 逐步补入 FreeList / MetaIndex。

namespace cabe {

    struct DeviceContext {
        int fd = -1;
        BufferPool pool{0};
    };

} // namespace cabe

#endif // CABE_DEVICE_CONTEXT_H
