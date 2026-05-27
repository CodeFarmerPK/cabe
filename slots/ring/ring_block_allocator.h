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
        // 当前 Init 从块 0 开始。P5 引入超级块后，设备头部将独立预留为
        // 元数据区域（非数据块），届时需调整起始块号或偏移计算。
        // 详见 doc/P4.5/README.md "关键技术备忘" 第 7 条。
        BlockId* slots_ = nullptr;
        std::size_t capacity_ = 0;
        std::size_t head_ = 0;
        std::size_t tail_ = 0;
    };

    static_assert(BlockAllocator<RingBlockAllocator>);

} // namespace cabe

#endif // CABE_RING_BLOCK_ALLOCATOR_H
