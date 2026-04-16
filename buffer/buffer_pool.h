/*
 * Project: Cabe
 * Created Time: 2026-04-16 20:07
 * Created by: CodeFarmerPK
 *
 * BufferPool — mmap-based fixed-size buffer pool
 *
 * Design:
 *   - Init() 通过 mmap 一次性分配 bufferCount 个大小为 bufferSize 的连续内存块
 *   - mmap 返回页对齐(4096)地址，天然满足 O_DIRECT 的 512 字节对齐要求
 *   - 使用栈(vector<uint32_t>)管理空闲索引，Acquire/Release 均为 O(1)
 *   - RAII: 析构函数自动 munmap，禁用拷贝和移动防止资源重复释放
 */

#ifndef CABE_BUFFER_POOL_H
#define CABE_BUFFER_POOL_H

#include "common/error_code.h"
#include <cstddef>
#include <cstdint>
#include <vector>

class BufferPool {
public:
    BufferPool() = default;
    ~BufferPool();

    // 禁用拷贝和移动
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    BufferPool(BufferPool&&) = delete;
    BufferPool& operator=(BufferPool&&) = delete;

    // 初始化：分配 bufferCount 个大小为 bufferSize 的缓冲区
    int32_t Init(size_t bufferSize, uint32_t bufferCount);

    // 销毁：释放所有内存
    int32_t Destroy();

    // 获取一个缓冲区（已清零），池耗尽时返回 nullptr
    char* Acquire();

    // 归还一个缓冲区
    int32_t Release(char* ptr);

    // 状态查询
    bool IsInitialized() const {
        return basePtr_ != nullptr;
    }
    uint32_t TotalCount() const {
        return bufferCount_;
    }
    uint32_t FreeCount() const {
        return static_cast<uint32_t>(freeStack_.size());
    }
    uint32_t UsedCount() const {
        return bufferCount_ - FreeCount();
    }
    size_t BufferSize() const {
        return bufferSize_;
    }

private:
    char* basePtr_ = nullptr; // mmap 返回的基地址
    size_t bufferSize_ = 0; // 单个缓冲区大小
    uint32_t bufferCount_ = 0; // 缓冲区总数
    size_t totalSize_ = 0; // 总分配大小 = bufferSize_ * bufferCount_
    std::vector<uint32_t> freeStack_; // 空闲缓冲区索引栈
};

#endif // CABE_BUFFER_POOL_H
