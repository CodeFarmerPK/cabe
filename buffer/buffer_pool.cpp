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

    // O_DIRECT 对单次 I/O 的地址和长度都有对齐要求（典型 512 字节）。
    // mmap 返回的 basePtr_ 是 4 KiB 页对齐，基址不是问题；但每个 buffer
    // 的偏移 = index * bufferSize_，如果 bufferSize_ 不是 512 的倍数，
    // 偏移 buffer 的地址就可能破坏对齐 → O_DIRECT 返回 EINVAL。
    // 当前唯一调用点用 1 MiB 天然满足；P3 引入小尺寸 buffer（如 4 KiB
    // SQE 数据缓冲）时这条检查会兜住潜在 bug。
    constexpr size_t kDirectIOAlignment = 512;
    if (bufferSize < kDirectIOAlignment || (bufferSize & (kDirectIOAlignment - 1)) != 0) {
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

    // 初始化空闲栈：倒序压入，使 pop_back 按地址顺序分配。
    // reserve 在内存极度紧张时可能抛 bad_alloc。Init 的契约是"返回错误码、
    // 不抛异常"，所以这里 try/catch 兜住，并把已 mmap 的内存释放掉，
    // 否则 mmap 区会一直挂在进程地址空间里直到 Engine 析构。
    try {
        freeStack_.reserve(bufferCount_);
        for (uint32_t i = bufferCount_; i > 0; --i) {
            freeStack_.push_back(i - 1);  // reserve 之后对 uint32_t 是 noexcept
        }
    } catch (...) {
        munmap(basePtr_, totalSize_);
        basePtr_ = nullptr;
        bufferSize_ = 0;
        bufferCount_ = 0;
        totalSize_ = 0;
        freeStack_.clear();
        // 不调 freeStack_.shrink_to_fit()：标准未保证 shrink_to_fit 是 noexcept
        // （理论上可能再次 bad_alloc 重新分配到 0 容量），catch 块内 throw 会
        // terminate。clear 之后保留少量 capacity 对接下来"BufferPool 实例本身
        // 也将被销毁/重置"无害。
        return POOL_MMAP_FAILED;
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

    // munmap 在合理使用下不会失败，但失败的话 mmap 区仍然有效，
    // 我们却把 basePtr_ 清零会让 BufferPool 永久失去对该内存的句柄
    // → 永久泄漏。把失败码透传给调用方，由它决定是否报警 / abort。
    const int rc = munmap(basePtr_, totalSize_);

    basePtr_ = nullptr;
    bufferSize_ = 0;
    bufferCount_ = 0;
    totalSize_ = 0;
    freeStack_.clear();
    // 不调 freeStack_.shrink_to_fit()：标准未保证 noexcept（理论上可能再次
    // bad_alloc 重新分配到 0 容量）。Destroy 由 ~BufferPool 调用，析构期 throw
    // → terminate。clear 后 vector 自身的堆内存在 BufferPool 实例销毁时由
    // vector 自己的析构释放，无需提前 shrink。

    if (rc != 0) {
        // 复用 POOL_MMAP_FAILED 表示 mmap 系列调用失败
        return POOL_MMAP_FAILED;
    }
    return SUCCESS;
}

// ============================================================
// Acquire: 从空闲栈弹出一个索引，返回对应缓冲区指针（已清零）
//
// 复杂度: O(1) (pop_back) + O(N) (memset, N=bufferSize)
// ============================================================
char* BufferPool::Acquire() {
    if (basePtr_ == nullptr ) {
        return nullptr;
    }

    uint32_t index;
    {
        std::lock_guard<std::mutex> lock(stackMutex_);
        if (freeStack_.empty()) {
            return nullptr;
        }
        index = freeStack_.back();
        freeStack_.pop_back();
    }

    // memset 在锁外执行：清零耗时，不应持锁阻塞其他线程 Acquire/Release
    char* buffer = basePtr_ + static_cast<size_t>(index) * bufferSize_;
    std::memset(buffer, 0, bufferSize_);
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
    std::lock_guard<std::mutex> lock(stackMutex_);
    // Double-release 检测：若 index 已在 freeStack_ 里，说明调用方重复
    // 释放同一 buffer。放过去会导致后续两个 Acquire 拿到同一块内存，
    // 造成静默 data corruption。
    // O(N) 扫描，N = bufferCount（默认 8），开销可忽略。
    for (const uint32_t existing : freeStack_) {
        if (existing == index) {
            return POOL_INVALID_POINTER;
        }
    }
    freeStack_.push_back(index);
    return SUCCESS;
}
