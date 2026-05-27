#ifndef CABE_BLOCK_ALLOCATOR_H
#define CABE_BLOCK_ALLOCATOR_H

#include "common/structs.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cabe {

    template<typename T>
    concept BlockAllocator = requires(T& alloc, const T& calloc,
                                       DeviceId dev, std::uint64_t block_count,
                                       BlockId id, BlockId* out,
                                       std::span<const BlockId> active_blocks) {
        { alloc.Init(dev, block_count) }                             -> std::same_as<int32_t>;
        { alloc.Acquire(out) }                                       -> std::same_as<int32_t>;
        { alloc.Recycle(id) }                                        -> std::same_as<void>;
        { calloc.Available() }                                       -> std::convertible_to<std::size_t>;
        { calloc.Empty() }                                           -> std::same_as<bool>;
        { alloc.RebuildFromActive(dev, block_count, active_blocks) } -> std::same_as<int32_t>;
    };

} // namespace cabe

#endif // CABE_BLOCK_ALLOCATOR_H
