#ifndef CABE_FREE_LIST_H
#define CABE_FREE_LIST_H

#include "common/structs.h"
#include "common/error_code.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cabe {

    class FreeList {
    public:
        FreeList() = default;

        int32_t Init(DeviceId dev, std::uint64_t block_count);

        int32_t Allocate(BlockId* out);

        void Free(BlockId id);

        std::size_t available() const noexcept;
        bool empty() const noexcept;

    private:
        std::vector<BlockId> stack_;
    };

} // namespace cabe

#endif // CABE_FREE_LIST_H
