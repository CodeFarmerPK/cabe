#ifndef CABE_PUT_PATH_H
#define CABE_PUT_PATH_H

#include "common/structs.h"

#include <cstddef>
#include <cstdint>

namespace cabe {

    enum class PutValueSourceKind : std::uint8_t {
        ExternalValueMemory = 0,
        TargetKeyValueBuffer = 1,
        OtherKeyValueBuffer = 2,
        OtherDeviceValueBuffer = 3,
        PoolAddressNotAllocated = 4,
    };

    struct PutValueSource {
        PutValueSourceKind kind = PutValueSourceKind::ExternalValueMemory;
        DeviceId owner_device_id = 0;
        std::uint64_t pool_id = 0;
        std::uint32_t slot_index = 0;
        std::uint32_t slot_generation = 0;
    };

    enum class PutWritePath : std::uint8_t {
        Direct = 0,
        CopyFallback = 1,
    };

    struct PutWritePlan {
        PutWritePath path = PutWritePath::CopyFallback;
    };

    struct BackendDirectWriteCaps {
        bool allow_external_value_memory = true;
        std::size_t external_memory_alignment = 4096;
    };

    bool IsAddressAligned(const void* ptr, std::size_t alignment) noexcept;

    PutWritePlan BuildPutWritePlan(DataView value,
                                    const PutValueSource& source,
                                    const BackendDirectWriteCaps& caps) noexcept;

} // namespace cabe

#endif // CABE_PUT_PATH_H
