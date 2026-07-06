#include "engine/put_path.h"

#include <cstdint>

namespace cabe {

    bool IsAddressAligned(const void* ptr, std::size_t alignment) noexcept {
        if (ptr == nullptr || alignment == 0) return false;
        const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
        return addr % alignment == 0;
    }

    PutWritePlan BuildPutWritePlan(DataView value,
                                    const PutValueSource& source,
                                    const BackendDirectWriteCaps& caps) noexcept {
        if (value.size() != kValueSize || value.data() == nullptr) {
            return {PutWritePath::CopyFallback};
        }

        switch (source.kind) {
        case PutValueSourceKind::TargetKeyValueBuffer:
            return {PutWritePath::Direct};
        case PutValueSourceKind::ExternalValueMemory:
            if (caps.allow_external_value_memory &&
                IsAddressAligned(value.data(), caps.external_memory_alignment)) {
                return {PutWritePath::Direct};
            }
            return {PutWritePath::CopyFallback};
        case PutValueSourceKind::OtherKeyValueBuffer:
        case PutValueSourceKind::OtherDeviceValueBuffer:
        case PutValueSourceKind::PoolAddressNotAllocated:
        default:
            return {PutWritePath::CopyFallback};
        }
    }

} // namespace cabe
