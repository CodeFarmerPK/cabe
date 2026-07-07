#ifndef CABE_IO_WRITE_BUFFER_H
#define CABE_IO_WRITE_BUFFER_H

#include "common/structs.h"

#include <cstddef>
#include <cstdint>

namespace cabe {

    enum class IoWriteBufferKind : std::uint8_t {
        ExternalMemory = 0,
        ValueBufferSlot = 1,
        CopyFallbackBuffer = 2,
    };

    struct ValueBufferSlotView {
        const std::byte* data = nullptr;
        std::size_t size = 0;
        std::uint32_t slot_index = 0;
    };

    struct IoWriteBuffer {
        const std::byte* data = nullptr;
        std::size_t size = 0;

        IoWriteBufferKind kind = IoWriteBufferKind::ExternalMemory;

        DeviceId device_id = 0;
        std::uint64_t pool_id = 0;
        std::uint32_t slot_index = 0;
        std::uint32_t slot_generation = 0;
    };

} // namespace cabe

#endif // CABE_IO_WRITE_BUFFER_H
