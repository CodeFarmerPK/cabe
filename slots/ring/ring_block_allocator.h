#ifndef CABE_RING_BLOCK_ALLOCATOR_H
#define CABE_RING_BLOCK_ALLOCATOR_H

#include "slots/block_allocator.h"
#include "common/error_code.h"
#include "common/structs.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace cabe {

    class RingBlockAllocator {
    public:
        RingBlockAllocator() noexcept = default;
        ~RingBlockAllocator();

        RingBlockAllocator(const RingBlockAllocator&) = delete;
        RingBlockAllocator& operator=(const RingBlockAllocator&) = delete;
        RingBlockAllocator(RingBlockAllocator&& other) noexcept;
        RingBlockAllocator& operator=(RingBlockAllocator&& other) noexcept;

        int32_t Init(DeviceId dev, std::uint64_t block_count);
        int32_t Acquire(BlockId* out);
        void Recycle(BlockId id);
        std::size_t Available() const noexcept;
        bool Empty() const noexcept;
        int32_t RebuildFromActive(DeviceId dev, std::uint64_t block_count,
                                   std::span<const BlockId> active_blocks);

    private:
        // 逻辑块从 0 起。设备头部 8K 为双份超级块（P5，bcache 风格），
        // 数据区物理偏移由 IoBackend 加 kDataRegionOffset 处理——分配器不感知超级块。
        BlockId* slots_ = nullptr;
        std::size_t capacity_ = 0;
        std::size_t head_ = 0;
        std::size_t tail_ = 0;
    };

    static_assert(BlockAllocator<RingBlockAllocator>);

} // namespace cabe

#endif // CABE_RING_BLOCK_ALLOCATOR_H
