#ifndef CABE_DEVICE_CONTEXT_H
#define CABE_DEVICE_CONTEXT_H

#include "engine/backend_config.h"
#include "engine/buffer_pool.h"
#include "engine/free_list.h"

namespace cabe {

    struct DeviceContext {
        IoBackendImpl io;
        BufferPool pool{0};
        FreeList free_list;
        MetaIndexImpl meta_index;
    };

} // namespace cabe

#endif // CABE_DEVICE_CONTEXT_H
