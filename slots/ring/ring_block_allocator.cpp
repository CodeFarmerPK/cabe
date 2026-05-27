#include "slots/ring/ring_block_allocator.h"

#include <vector>

namespace cabe {

    RingBlockAllocator::~RingBlockAllocator() {
        delete[] slots_;
    }

    RingBlockAllocator::RingBlockAllocator(RingBlockAllocator&& other) noexcept
        : slots_(other.slots_)
        , capacity_(other.capacity_)
        , head_(other.head_)
        , tail_(other.tail_) {
        other.slots_ = nullptr;
        other.capacity_ = 0;
        other.head_ = 0;
        other.tail_ = 0;
    }

    RingBlockAllocator& RingBlockAllocator::operator=(RingBlockAllocator&& other) noexcept {
        if (this != &other) {
            delete[] slots_;

            slots_ = other.slots_;
            capacity_ = other.capacity_;
            head_ = other.head_;
            tail_ = other.tail_;

            other.slots_ = nullptr;
            other.capacity_ = 0;
            other.head_ = 0;
            other.tail_ = 0;
        }
        return *this;
    }

    int32_t RingBlockAllocator::Init(DeviceId dev, std::uint64_t block_count) {
        delete[] slots_;
        capacity_ = block_count + 1;
        slots_ = new BlockId[capacity_];
        head_ = 0;
        tail_ = 0;

        for (std::uint64_t i = 0; i < block_count; ++i) {
            slots_[tail_] = BlockId::Make(dev, i);
            tail_ = (tail_ + 1) % capacity_;
        }
        return err::kSuccess;
    }

    int32_t RingBlockAllocator::Acquire(BlockId* out) {
        if (head_ == tail_) return err::kEngineNoSpace;
        *out = slots_[head_];
        head_ = (head_ + 1) % capacity_;
        return err::kSuccess;
    }

    void RingBlockAllocator::Recycle(BlockId id) {
        slots_[tail_] = id;
        tail_ = (tail_ + 1) % capacity_;
    }

    std::size_t RingBlockAllocator::Available() const noexcept {
        return (tail_ - head_ + capacity_) % capacity_;
    }

    bool RingBlockAllocator::Empty() const noexcept {
        return head_ == tail_;
    }

    int32_t RingBlockAllocator::RebuildFromActive(DeviceId dev, std::uint64_t block_count,
                                                    std::span<const BlockId> active_blocks) {
        delete[] slots_;
        capacity_ = block_count + 1;
        slots_ = new BlockId[capacity_];
        head_ = 0;
        tail_ = 0;

        std::vector<bool> used(block_count, false);
        for (const auto& bid : active_blocks) {
            used[bid.block_idx()] = true;
        }

        for (std::uint64_t i = 0; i < block_count; ++i) {
            if (!used[i]) {
                slots_[tail_] = BlockId::Make(dev, i);
                tail_ = (tail_ + 1) % capacity_;
            }
        }
        return err::kSuccess;
    }

} // namespace cabe
