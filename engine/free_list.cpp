#include "engine/free_list.h"

namespace cabe {

    int32_t FreeList::Init(DeviceId dev, std::uint64_t block_count) {
        stack_.clear();
        stack_.reserve(block_count);
        for (std::uint64_t i = block_count; i > 0; --i) {
            stack_.push_back(BlockId::Make(dev, i - 1));
        }
        return err::kSuccess;
    }

    int32_t FreeList::Allocate(BlockId* out) {
        if (stack_.empty()) return err::kEngineNoSpace;
        *out = stack_.back();
        stack_.pop_back();
        return err::kSuccess;
    }

    void FreeList::Free(BlockId id) {
        stack_.push_back(id);
    }

    std::size_t FreeList::available() const noexcept {
        return stack_.size();
    }

    bool FreeList::empty() const noexcept {
        return stack_.empty();
    }

} // namespace cabe
