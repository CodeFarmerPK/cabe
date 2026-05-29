#ifndef CABE_DEVICE_CONTEXT_H
#define CABE_DEVICE_CONTEXT_H

#include "engine/backend_config.h"
#include "engine/buffer_pool.h"
#include "engine/super_block.h"

namespace cabe {

    struct DeviceContext {
        IoBackendImpl io;
        BufferPool pool{0};
        BlockAllocatorImpl block_allocator;
        MetaIndexImpl meta_index;
        SuperBlock super_block{};   // 本数据设备的超级块（create 写入 / recover 读入后保存）
    };

} // namespace cabe

#endif // CABE_DEVICE_CONTEXT_H
