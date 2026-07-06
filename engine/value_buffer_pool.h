#ifndef CABE_VALUE_BUFFER_POOL_H
#define CABE_VALUE_BUFFER_POOL_H

#include "common/structs.h"
#include "engine/status.h"
#include "engine/value_buffer.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace cabe {

    namespace detail {
        struct ValueBufferPoolState;
    }

    class ValueBufferPool {
    public:
        struct CreateResult {
            Status status;
            std::shared_ptr<ValueBufferPool> pool;

            bool ok() const noexcept;
        };

        struct SourceInfo {
            bool matched = false;
            DeviceId device_id = 0;
            std::uint64_t pool_id = 0;
            std::uint32_t slot_index = 0;
            std::uint32_t slot_generation = 0;
        };

        static CreateResult Create(DeviceId device_id, std::uint64_t pool_id,
                                   std::size_t slot_count);

        ValueBufferResult Allocate();
        SourceInfo Identify(DataView value) const noexcept;

        void BeginClose() noexcept;
        void WaitUntilIdle() noexcept;

        DeviceId device_id() const noexcept;
        std::uint64_t pool_id() const noexcept;
        std::size_t slot_count() const noexcept;

    private:
        explicit ValueBufferPool(std::shared_ptr<detail::ValueBufferPoolState> state) noexcept;

        std::shared_ptr<detail::ValueBufferPoolState> state_;
    };

} // namespace cabe

#endif // CABE_VALUE_BUFFER_POOL_H
