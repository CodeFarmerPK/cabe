#ifndef CABE_VALUE_BUFFER_POOL_H
#define CABE_VALUE_BUFFER_POOL_H

#include "common/structs.h"
#include "engine/status.h"
#include "engine/value_buffer.h"
#include "wal/wal_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace cabe {

    namespace detail {
        struct ValueBufferPoolState;
    }

    class ValueBufferPool {
    public:
        enum class ProbeKind : std::uint8_t {
            NotInPool = 0,
            AllocatedSlot = 1,
            PoolAddressNotAllocated = 2,
        };

        struct CreateResult {
            Status status;
            std::shared_ptr<ValueBufferPool> pool;

            bool ok() const noexcept;
        };

        struct SourceInfo {
            ProbeKind kind = ProbeKind::NotInPool;
            bool matched = false;
            DeviceId device_id = 0;
            std::uint64_t pool_id = 0;
            std::uint32_t slot_index = 0;
            std::uint32_t slot_generation = 0;
            std::uint16_t bound_key_len = 0;
            std::array<char, kWalKeyMax> bound_key_bytes{};

            std::string_view bound_key() const noexcept;
            bool BoundKeyEquals(std::string_view key) const noexcept;
        };

        static CreateResult Create(DeviceId device_id, std::uint64_t pool_id,
                                   std::size_t slot_count);

        ValueBufferResult Allocate(std::string_view bound_key);
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
