/*
 * Project: Cabe
 * Created Time: 2026-04-16 20:07
 * Created by: CodeFarmerPK
 */

#include "buffer_pool.h"

#include <cstring>
#include <sys/mman.h>

// ============================================================
// 析构：RAII 保障，防止忘记调用 Destroy() 导致内存泄漏
// ============================================================
BufferPool::~BufferPool() {
    if (basePtr_ != nullptr) {
        Destroy();
    }
}

// ============================================================
// Init: 通过 mmap 一次性分配所有缓冲区
//
// 关键点:
//   - mmap 返回页对齐(4096)地址 → 自动满足 O_DIRECT 512 对齐
//   - MAP_POPULATE 立刻分配物理页 → 避免运行时缺页中断
//   - 空闲栈倒序压入 [N-1, N-2, ..., 1, 0]
//     → pop_back 时先取 0, 再取 1 ... 按地址顺序分配
// ============================================================
int32_t BufferPool::Init(const size_t bufferSize, const uint32_t bufferCount) {
    if (basePtr_ != nullptr) {
        return POOL_ALREADY_INITIALIZED;
    }
    if (bufferSize == 0 || bufferCount == 0) {
        return POOL_INVALID_PARAMS;
    }

    bufferSize_ = bufferSize;
    bufferCount_ = bufferCount;
    totalSize_ = bufferSize_ * bufferCount_;

    // mmap 分配连续内存
    void* ptr = mmap(nullptr, // 内核自动选择地址
        totalSize_, // 总大小
        PROT_READ | PROT_WRITE, // 可读可写
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, // 私有匿名映射 + 预分配物理页
        -1, // 无关联文件
        0 // 偏移量
    );

    if (ptr == MAP_FAILED) {
        bufferSize_ = 0;
        bufferCount_ = 0;
        totalSize_ = 0;
        return POOL_MMAP_FAILED;
    }

    basePtr_ = static_cast<char*>(ptr);

    // 初始化空闲栈：倒序压入，使 pop_back 按地址顺序分配
    freeStack_.reserve(bufferCount_);
    for (uint32_t i = bufferCount_; i > 0; --i) {
        freeStack_.push_back(i - 1);
    }

    return SUCCESS;
}

// ============================================================
// Destroy: munmap 释放全部内存，重置所有状态
// ============================================================
int32_t BufferPool::Destroy() {
    if (basePtr_ == nullptr) {
        return POOL_NOT_INITIALIZED;
    }

    munmap(basePtr_, totalSize_);

    basePtr_ = nullptr;
    bufferSize_ = 0;
    bufferCount_ = 0;
    totalSize_ = 0;
    freeStack_.clear();
    freeStack_.shrink_to_fit(); // 释放 vector 自身的堆内存

    return SUCCESS;
}

// ============================================================
// Acquire: 从空闲栈弹出一个索引，返回对应缓冲区指针（已清零）
//
// 复杂度: O(1) (pop_back) + O(N) (memset, N=bufferSize)
// ============================================================
char* BufferPool::Acquire() {
    if (basePtr_ == nullptr || freeStack_.empty()) {
        return nullptr;
    }

    const uint32_t index = freeStack_.back();
    freeStack_.pop_back();

    char* buffer = basePtr_ + static_cast<size_t>(index) * bufferSize_;
    std::memset(buffer, 0, bufferSize_); // 清零，与原 AllocateAlignedBuffer 行为一致

    return buffer;
}

// ============================================================
// Release: 验证指针合法性后，将索引压回空闲栈
//
// 验证步骤:
//   1. 池是否已初始化
//   2. 指针是否为空
//   3. 指针是否在 [basePtr_, basePtr_ + totalSize_) 范围内
//   4. 指针偏移是否对齐到 bufferSize_ 边界
// ============================================================
int32_t BufferPool::Release(char* ptr) {
    if (basePtr_ == nullptr) {
        return POOL_NOT_INITIALIZED;
    }
    if (ptr == nullptr) {
        return POOL_INVALID_POINTER;
    }

    // 范围检查
    if (ptr < basePtr_ || ptr >= basePtr_ + totalSize_) {
        return POOL_INVALID_POINTER;
    }

    // 对齐检查：偏移量必须是 bufferSize_ 的整数倍
    const auto offset = static_cast<size_t>(ptr - basePtr_);
    if (offset % bufferSize_ != 0) {
        return POOL_INVALID_POINTER;
    }

    const auto index = static_cast<uint32_t>(offset / bufferSize_);
    freeStack_.push_back(index);

    return SUCCESS;
}
