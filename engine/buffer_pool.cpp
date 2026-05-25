#include "engine/buffer_pool.h"
#include "common/logger.h"

#include <cstdlib>
#include <utility>

namespace cabe {

    BufferPool::BufferPool(std::size_t block_count)
        : block_count_(block_count) {
        if (block_count_ == 0) return;
        base_ = static_cast<std::byte*>(
            std::aligned_alloc(kPageSize, block_count_ * kValueSize));
        if (!base_) {
            CABE_LOG_FATAL("BufferPool: aligned_alloc(%zu, %zu) 失败",
                           kPageSize, block_count_ * kValueSize);
            std::abort();
        }
        free_list_.reserve(block_count_);
        for (std::size_t i = 0; i < block_count_; ++i) {
            free_list_.push_back(base_ + i * kValueSize);
        }
    }

    BufferPool::~BufferPool() {
        if (base_) {
            if (free_list_.size() != block_count_) {
                CABE_LOG_WARN("BufferPool 析构时仍有 %zu 块未归还",
                              block_count_ - free_list_.size());
            }
            std::free(base_);
            base_ = nullptr;
        }
    }

    BufferPool::BufferPool(BufferPool&& other) noexcept
        : base_(other.base_)
        , block_count_(other.block_count_)
        , free_list_(std::move(other.free_list_)) {
        other.base_ = nullptr;
        other.block_count_ = 0;
    }

    BufferPool& BufferPool::operator=(BufferPool&& other) noexcept {
        if (this != &other) {
            if (base_) std::free(base_);
            base_ = other.base_;
            block_count_ = other.block_count_;
            free_list_ = std::move(other.free_list_);
            other.base_ = nullptr;
            other.block_count_ = 0;
        }
        return *this;
    }

    std::byte* BufferPool::Allocate() {
        if (free_list_.empty()) return nullptr;
        std::byte* buf = free_list_.back();
        free_list_.pop_back();
        return buf;
    }

    void BufferPool::Free(std::byte* buf) {
        free_list_.push_back(buf);
    }

    std::size_t BufferPool::available() const noexcept {
        return free_list_.size();
    }

    std::size_t BufferPool::capacity() const noexcept {
        return block_count_;
    }

} // namespace cabe
