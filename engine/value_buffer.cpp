#include "engine/value_buffer.h"

#include <utility>

namespace cabe {

    namespace detail {
        struct ValueBufferControlBlock {
            virtual ~ValueBufferControlBlock() = default;
        };
    } // namespace detail

    ValueBuffer::ValueBuffer() noexcept = default;

    ValueBuffer::~ValueBuffer() = default;

    ValueBuffer::ValueBuffer(ValueBuffer&& other) noexcept
        : control_(std::move(other.control_))
        , data_(other.data_) {
        other.data_ = nullptr;
    }

    ValueBuffer& ValueBuffer::operator=(ValueBuffer&& other) noexcept {
        if (this != &other) {
            control_ = std::move(other.control_);
            data_ = other.data_;
            other.data_ = nullptr;
        }
        return *this;
    }

    ValueBuffer::ValueBuffer(std::shared_ptr<detail::ValueBufferControlBlock> control,
                             std::byte* data) noexcept
        : control_(std::move(control))
        , data_(data) {}

    DataBuffer ValueBuffer::data() noexcept {
        if (!valid()) return {};
        return DataBuffer{data_, kValueSize};
    }

    DataView ValueBuffer::view() const noexcept {
        if (!valid()) return {};
        return DataView{data_, kValueSize};
    }

    bool ValueBuffer::valid() const noexcept {
        return control_ != nullptr && data_ != nullptr;
    }

    bool ValueBufferResult::ok() const noexcept {
        return status.ok();
    }

} // namespace cabe
