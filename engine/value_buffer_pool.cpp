#include "engine/value_buffer_pool.h"
#include "common/error_code.h"

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>

namespace cabe {

    namespace {
        constexpr std::uint32_t kInvalidSlot = std::numeric_limits<std::uint32_t>::max();

        enum class SlotState : std::uint8_t {
            Free = 0,
            Allocated = 1,
        };

        constexpr std::uint64_t PackHead(std::uint32_t tag, std::uint32_t index) noexcept {
            return (static_cast<std::uint64_t>(tag) << 32) | index;
        }

        constexpr std::uint32_t HeadTag(std::uint64_t head) noexcept {
            return static_cast<std::uint32_t>(head >> 32);
        }

        constexpr std::uint32_t HeadIndex(std::uint64_t head) noexcept {
            return static_cast<std::uint32_t>(head & 0xFFFFFFFFu);
        }
    } // namespace

    namespace detail {
        struct ValueBufferSlot {
            std::atomic<std::uint32_t> next_free{kInvalidSlot};
            std::atomic<std::uint8_t> state{static_cast<std::uint8_t>(SlotState::Free)};
            std::atomic<std::uint32_t> generation{1};
            std::byte* data = nullptr;
        };

        struct ValueBufferPoolState {
            DeviceId device_id = 0;
            std::uint64_t pool_id = 0;
            std::size_t slot_count = 0;
            std::size_t total_size = 0;
            std::byte* base = nullptr;
            std::unique_ptr<ValueBufferSlot[]> slots;

            std::atomic<bool> closing{false};
            std::atomic<std::uint32_t> active_count{0};
            std::atomic<std::uint64_t> free_head{PackHead(0, kInvalidSlot)};

            ~ValueBufferPoolState() {
                std::free(base);
            }

            void DropActiveRef() noexcept {
                const std::uint32_t old = active_count.fetch_sub(1, std::memory_order_acq_rel);
                assert(old > 0);
                (void)old;
                if (closing.load(std::memory_order_acquire)) {
                    active_count.notify_all();
                }
            }

            void PushFreeSlot(std::uint32_t index) noexcept {
                while (true) {
                    std::uint64_t head = free_head.load(std::memory_order_acquire);
                    slots[index].next_free.store(HeadIndex(head), std::memory_order_relaxed);
                    const std::uint64_t desired = PackHead(HeadTag(head) + 1, index);
                    if (free_head.compare_exchange_weak(
                            head, desired, std::memory_order_release, std::memory_order_acquire)) {
                        return;
                    }
                }
            }

            void ReleaseSlot(std::uint32_t index, std::uint32_t generation) noexcept {
                if (index >= slot_count || slots == nullptr) {
                    DropActiveRef();
                    return;
                }

                ValueBufferSlot& slot = slots[index];
                if (slot.generation.load(std::memory_order_acquire) != generation) {
                    DropActiveRef();
                    return;
                }

                const std::uint8_t old_state = slot.state.exchange(
                    static_cast<std::uint8_t>(SlotState::Free), std::memory_order_acq_rel);
                if (old_state != static_cast<std::uint8_t>(SlotState::Allocated)) {
                    DropActiveRef();
                    return;
                }

                slot.generation.fetch_add(1, std::memory_order_acq_rel);
                PushFreeSlot(index);
                DropActiveRef();
            }
        };
    } // namespace detail

    namespace {
        class ValueBufferSlotControlBlock final : public detail::ValueBufferControlBlock {
        public:
            ValueBufferSlotControlBlock(std::shared_ptr<detail::ValueBufferPoolState> state,
                                        std::uint32_t slot_index,
                                        std::uint32_t slot_generation) noexcept
                : state_(std::move(state))
                , slot_index_(slot_index)
                , slot_generation_(slot_generation) {}

            ~ValueBufferSlotControlBlock() override {
                Release();
            }

            void Release() noexcept override {
                if (released_.exchange(true, std::memory_order_acq_rel)) return;
                if (state_) state_->ReleaseSlot(slot_index_, slot_generation_);
            }

            bool released() const noexcept override {
                return released_.load(std::memory_order_acquire);
            }

        private:
            std::shared_ptr<detail::ValueBufferPoolState> state_;
            std::uint32_t slot_index_ = kInvalidSlot;
            std::uint32_t slot_generation_ = 0;
            std::atomic<bool> released_{false};
        };

        bool FitsInSize(std::size_t slot_count) noexcept {
            return slot_count <= std::numeric_limits<std::size_t>::max() / kValueSize;
        }

        void FinishFailedAllocate(const std::shared_ptr<detail::ValueBufferPoolState>& state) noexcept {
            state->DropActiveRef();
        }
    } // namespace

    bool ValueBufferPool::CreateResult::ok() const noexcept {
        return status.ok();
    }

    ValueBufferPool::CreateResult ValueBufferPool::Create(DeviceId device_id, std::uint64_t pool_id,
                                                          std::size_t slot_count) {
        if (slot_count >= kInvalidSlot) {
            return {Status::Error(err::kEngineInvalidOpts), nullptr};
        }
        if (!FitsInSize(slot_count)) {
            return {Status::Error(err::kEngineInvalidOpts), nullptr};
        }

        std::shared_ptr<detail::ValueBufferPoolState> state;
        try {
            state = std::make_shared<detail::ValueBufferPoolState>();
        } catch (const std::bad_alloc&) {
            return {Status::Error(err::kEnginePoolExhausted), nullptr};
        }

        state->device_id = device_id;
        state->pool_id = pool_id;
        state->slot_count = slot_count;
        state->total_size = slot_count * kValueSize;

        if (slot_count > 0) {
            state->base = static_cast<std::byte*>(std::aligned_alloc(kValueSize, state->total_size));
            if (state->base == nullptr) {
                return {Status::Error(err::kEnginePoolExhausted), nullptr};
            }

            try {
                state->slots = std::make_unique<detail::ValueBufferSlot[]>(slot_count);
            } catch (const std::bad_alloc&) {
                return {Status::Error(err::kEnginePoolExhausted), nullptr};
            }

            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(slot_count); ++i) {
                state->slots[i].data = state->base + static_cast<std::size_t>(i) * kValueSize;
                state->slots[i].next_free.store(
                    (i + 1 == slot_count) ? kInvalidSlot : i + 1, std::memory_order_relaxed);
                state->slots[i].state.store(static_cast<std::uint8_t>(SlotState::Free),
                                            std::memory_order_relaxed);
                state->slots[i].generation.store(1, std::memory_order_relaxed);
            }
            state->free_head.store(PackHead(0, 0), std::memory_order_release);
        }

        try {
            return {Status::Ok(), std::shared_ptr<ValueBufferPool>(new ValueBufferPool(std::move(state)))};
        } catch (const std::bad_alloc&) {
            return {Status::Error(err::kEnginePoolExhausted), nullptr};
        }
    }

    ValueBufferPool::ValueBufferPool(std::shared_ptr<detail::ValueBufferPoolState> state) noexcept
        : state_(std::move(state)) {}

    ValueBufferResult ValueBufferPool::Allocate() {
        if (!state_ || state_->closing.load(std::memory_order_acquire)) {
            return {Status::Error(err::kEngineNotOpen), {}};
        }

        state_->active_count.fetch_add(1, std::memory_order_acq_rel);
        if (state_->closing.load(std::memory_order_acquire)) {
            FinishFailedAllocate(state_);
            return {Status::Error(err::kEngineNotOpen), {}};
        }

        while (true) {
            std::uint64_t head = state_->free_head.load(std::memory_order_acquire);
            const std::uint32_t index = HeadIndex(head);
            if (index == kInvalidSlot) {
                FinishFailedAllocate(state_);
                return {Status::Error(err::kEnginePoolExhausted), {}};
            }

            detail::ValueBufferSlot& slot = state_->slots[index];
            const std::uint32_t next = slot.next_free.load(std::memory_order_relaxed);
            const std::uint64_t desired = PackHead(HeadTag(head) + 1, next);
            if (!state_->free_head.compare_exchange_weak(
                    head, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                continue;
            }

            slot.state.store(static_cast<std::uint8_t>(SlotState::Allocated), std::memory_order_release);
            const std::uint32_t generation = slot.generation.load(std::memory_order_acquire);
            try {
                auto control = std::make_shared<ValueBufferSlotControlBlock>(state_, index, generation);
                return {Status::Ok(), ValueBuffer(std::move(control), slot.data)};
            } catch (const std::bad_alloc&) {
                state_->ReleaseSlot(index, generation);
                return {Status::Error(err::kEnginePoolExhausted), {}};
            }
        }
    }

    ValueBufferPool::SourceInfo ValueBufferPool::Identify(DataView value) const noexcept {
        SourceInfo info{};
        if (!state_ || value.size() != kValueSize || value.data() == nullptr || state_->slot_count == 0 ||
            state_->base == nullptr) {
            return info;
        }

        const auto addr = reinterpret_cast<std::uintptr_t>(value.data());
        const auto base = reinterpret_cast<std::uintptr_t>(state_->base);
        const auto end = base + state_->total_size;
        if (addr < base || addr >= end) return info;

        const std::uintptr_t offset = addr - base;
        if (offset % kValueSize != 0) return info;

        const std::size_t slot_index = offset / kValueSize;
        if (slot_index >= state_->slot_count) return info;

        const detail::ValueBufferSlot& slot = state_->slots[slot_index];
        if (slot.state.load(std::memory_order_acquire) !=
            static_cast<std::uint8_t>(SlotState::Allocated)) {
            return info;
        }

        info.matched = true;
        info.device_id = state_->device_id;
        info.pool_id = state_->pool_id;
        info.slot_index = static_cast<std::uint32_t>(slot_index);
        info.slot_generation = slot.generation.load(std::memory_order_acquire);
        return info;
    }

    void ValueBufferPool::BeginClose() noexcept {
        if (!state_) return;
        state_->closing.store(true, std::memory_order_release);
        state_->active_count.notify_all();
    }

    void ValueBufferPool::WaitUntilIdle() noexcept {
        if (!state_) return;
        while (true) {
            const std::uint32_t count = state_->active_count.load(std::memory_order_acquire);
            if (count == 0) return;
            state_->active_count.wait(count, std::memory_order_acquire);
        }
    }

    DeviceId ValueBufferPool::device_id() const noexcept {
        return state_ ? state_->device_id : DeviceId{0};
    }

    std::uint64_t ValueBufferPool::pool_id() const noexcept {
        return state_ ? state_->pool_id : 0;
    }

    std::size_t ValueBufferPool::slot_count() const noexcept {
        return state_ ? state_->slot_count : 0;
    }

} // namespace cabe
