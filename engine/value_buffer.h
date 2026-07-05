#ifndef CABE_VALUE_BUFFER_H
#define CABE_VALUE_BUFFER_H

#include "common/structs.h"
#include "engine/status.h"

#include <memory>

namespace cabe {

    namespace detail {
        struct ValueBufferControlBlock;
    }

    class ValueBufferPool;

    class ValueBuffer {
    public:
        ValueBuffer() noexcept;
        ~ValueBuffer();

        ValueBuffer(ValueBuffer&& other) noexcept;
        ValueBuffer& operator=(ValueBuffer&& other) noexcept;

        ValueBuffer(const ValueBuffer&) = delete;
        ValueBuffer& operator=(const ValueBuffer&) = delete;

        DataBuffer data() noexcept;
        DataView view() const noexcept;
        bool valid() const noexcept;

    private:
        friend class Engine;
        friend class ValueBufferPool;

        ValueBuffer(std::shared_ptr<detail::ValueBufferControlBlock> control,
                    std::byte* data) noexcept;

        std::shared_ptr<detail::ValueBufferControlBlock> control_;
        std::byte* data_ = nullptr;
    };

    struct ValueBufferResult {
        Status status;
        ValueBuffer buffer;

        bool ok() const noexcept;
    };

} // namespace cabe

#endif // CABE_VALUE_BUFFER_H
