#ifndef CABE_BUFFER_POOL_H
#define CABE_BUFFER_POOL_H

#include "common/structs.h"

#include <cstddef>
#include <vector>

namespace cabe {

    inline constexpr std::size_t kPageSize = 4096;
    inline constexpr std::size_t kDefaultPoolBlocks = 16;

    class BufferPool {
    public:
        explicit BufferPool(std::size_t block_count = kDefaultPoolBlocks);
        ~BufferPool();

        BufferPool(const BufferPool&) = delete;
        BufferPool& operator=(const BufferPool&) = delete;
        BufferPool(BufferPool&& other) noexcept;
        BufferPool& operator=(BufferPool&& other) noexcept;

        std::byte* Allocate();
        void Free(std::byte* buf);

        std::size_t available() const noexcept;
        std::size_t capacity() const noexcept;

    private:
        std::byte* base_ = nullptr;
        std::size_t block_count_ = 0;
        std::vector<std::byte*> free_list_;
    };

} // namespace cabe

#endif // CABE_BUFFER_POOL_H
