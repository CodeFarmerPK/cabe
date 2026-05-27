#ifndef CABE_DEVICE_CONTEXT_H
#define CABE_DEVICE_CONTEXT_H

#include "engine/backend_config.h"
#include "engine/buffer_pool.h"

namespace cabe {

    struct DeviceContext {
        IoBackendImpl io;
        BufferPool pool{0};
        BlockAllocatorImpl block_allocator;
        MetaIndexImpl meta_index;
    };

} // namespace cabe

#endif // CABE_DEVICE_CONTEXT_H
