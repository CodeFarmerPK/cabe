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
 *
 *   线程安全契约（重要，P3 多 NVMe 拆分前需重新评估）：
 *   - Acquire / Release 之间是线程安全的：内部 stackMutex_ 保护 freeStack_，
 *     memset 在锁外执行（每个 buffer 独占于 Acquire→Release 之间，无 race）
 *   - Init / Destroy 与 Acquire / Release 之间**不是**线程安全的：basePtr_ /
 *     totalSize_ / bufferSize_ 都是裸成员，无原子保护；且 Destroy 的 munmap
 *     会让正在 Acquire/Release 的 buffer 指针变成悬空（use-after-free）
 *   - 调用方必须保证：Init 完成后、Destroy 之前的窗口内才调用 Acquire/Release
 *   - 当前唯一调用方 ::Engine 通过 mutex_(unique_lock for Open/Close,
 *     shared/unique_lock for Acquire/Release) 已隐式满足该约束
 *
 * 仅靠让 basePtr_ 变 std::atomic<char*> 不能解决 Destroy↔Acquire 的 use-after-free
 * （Destroy 已 munmap 但 Acquire 已拿到旧 basePtr_ 指针），所以保持裸成员 +
 * 文档化生命周期契约。需要 BufferPool 跨 Engine 实例共享时，再加引用计数 +
 * RCU-style 延迟回收。
 */

#ifndef CABE_BUFFER_POOL_H
#define CABE_BUFFER_POOL_H

#include "common/error_code.h"
#include <cstddef>
#include <cstdint>
#include <mutex>
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
        std::lock_guard<std::mutex> lock(stackMutex_);
        return static_cast<uint32_t>(freeStack_.size());
    }
    uint32_t UsedCount() const {
        std::lock_guard<std::mutex> lock(stackMutex_);
        return bufferCount_ - static_cast<uint32_t>(freeStack_.size());
    }
    size_t BufferSize() const {
        return bufferSize_;
    }

private:
    mutable std::mutex stackMutex_; // 保护 freeStack_ 的并发 Acquire/Release
    char* basePtr_ = nullptr; // mmap 返回的基地址
    size_t bufferSize_ = 0; // 单个缓冲区大小
    uint32_t bufferCount_ = 0; // 缓冲区总数
    size_t totalSize_ = 0; // 总分配大小 = bufferSize_ * bufferCount_
    std::vector<uint32_t> freeStack_; // 空闲缓冲区索引栈
};

#endif // CABE_BUFFER_POOL_H
