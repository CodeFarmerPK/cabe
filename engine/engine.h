/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:31
 * Created by: CodeFarmerPK
 */

#ifndef CABE_ENGINE_H
#define CABE_ENGINE_H

#include "buffer/buffer_pool.h"
#include "common/error_code.h"
#include "common/structs.h"
#include "memory/chunk_index.h"
#include "memory/meta_index.h"
#include "storage/free_list.h"
#include "storage/storage.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <vector>

class Engine {
public:
    Engine() = default;
    ~Engine();

    // Engine 持有 BufferPool / Storage 等独占资源，语义上不允许拷贝或移动
    // （否则会出现两个 Engine 共享同一 fd / mmap 区的 UB）。
    // 显式 = delete 让错误调用在编译期就被发现，而不是传递到成员层面。
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    // bufferPoolCount：BufferPool 中的 1 MiB 对齐缓冲区数量（默认 8）
    //
    // 生命周期约束（P3 前）：
    //   每个 ::Engine 实例的 Open / Close 是 **一次性** 的。Close 后再 Open
    //   （同实例）不会重置 metaIndex_ / chunkIndex_ / freeList_ / nextChunkId_，
    //   会导致内存索引与新磁盘内容不一致 → 静默 corruption。
    //   想"重新打开"必须销毁 Engine 实例后构造新实例。
    //   cabe::Engine 公开 API 已通过工厂方法 + unique_ptr 强制此约束。
    //   P4 持久化引入后会重新设计该生命周期。
    int32_t Open(const std::string& devicePath, uint32_t bufferPoolCount = 8);
    // 关闭引擎
    int32_t Close();

    // 写入数据。value 大小不受限，自动按 CABE_VALUE_DATA_SIZE (1 MiB) 切 chunk。
    //
    // 性能契约：Cabe 的优化目标是 value 大小在 1 MiB 附近及以上的场景，
    // 此时单 chunk / 多 chunk 路径都能吃到硬件 CRC32C、O_DIRECT 顺序
    // 写和 BufferPool 对齐缓冲。小于 1 MiB 的 value 仍然能正确读写，
    // 但会走"单个不满 chunk"路径，1 MiB 定长块的写放大不变，
    // QPS 会被 O_DIRECT + O_SYNC 的延迟主导，不适合做高频小 KV。
    int32_t Put(const std::string& key, DataView data);

    // 读取数据（自动合并多个 chunk）。性能特性同 Put。
    int32_t Get(const std::string& key, DataBuffer buffer, uint64_t* readSize) const;

    // 读取数据到 vector（P2 API 层专用）。
    // 在单次 shared_lock 内完成：查 MetaIndex → resize vector → 读磁盘 → CRC 校验。
    // 避免"先查 size 再 Get"两次加锁之间的竞态（TOCTOU）。
    // 失败时 *out 被清空（clear()），调用方无需额外处理。
    int32_t GetIntoVector(const std::string& key, std::vector<std::byte>* out) const;

    // 删除数据（标记删除）
    int32_t Delete(const std::string& key);

    // 真正移除并回收空间
    int32_t Remove(const std::string& key);

    // 获取 key 数量
    size_t Size() const;

    // 是否已打开
    bool IsOpen() const;

private:
    // 分配连续的 chunkId，返回首个 chunkId
    ChunkId AllocateChunkIds(uint32_t count);


    // 粗粒度 RW 锁：保护所有 Engine 状态的组合操作原子性。
    // 写操作（Put/Delete/Remove/Open/Close）持 unique_lock；
    // 读操作（Get/Size/IsOpen）持 shared_lock，允许多读并发。
    // P3 引入 io_uring 后，Put 的 I/O 阶段将脱离锁保护，届时锁持有时间大幅缩短。
    // mutable：Get/Size/IsOpen 是 const 方法，但仍需获取 shared_lock。
    //
    // TODO(P3 多 NVMe)：当前 mutex_ 同时保护 metaIndex_/chunkIndex_/freeList_/storage_。
    //   多 NVMe 路线（roadmap）需要：
    //     1. freeList_ / storage_ 改为 std::vector<FreeList> / std::vector<Storage>，每设备一个
    //     2. 每个 FreeList / Storage 持自己的 mutex（去除对 Engine mutex 的依赖）
    //     3. ChunkId → DeviceIndex 的策略层加在 AllocateChunkIds 上
    //     4. mutex_ 缩小到只保护 metaIndex_ / chunkIndex_
    //   这样单 NVMe 性能不变，多 NVMe 时各设备的 I/O / FreeList 操作可真并行。
    mutable std::shared_mutex mutex_;
    MetaIndex metaIndex_; // 第一层: key(string) → KeyMeta{firstChunkId, chunkCount}
    ChunkIndex chunkIndex_; // 第二层: chunkId → ChunkMeta (std::map, 有序)
    FreeList freeList_; // 磁盘块分配
    Storage storage_; // 磁盘 I/O

    // mutable: Get() 是 const 方法，但需要 Acquire/Release 修改池内部状态
    // 语义正确：Get 不改变引擎的「逻辑状态」，池是内部实现细节
    // BufferPool 内部持有独立 mutex，不依赖 Engine mutex 保护。
    mutable BufferPool bufferPool_;

    // fetch_add 原子自增，P3 并行写入时无需持 Engine 锁即可分配 chunkId
    std::atomic<ChunkId> nextChunkId_{0};
    bool isOpen_ = false;

    // 记录 Open 时使用的路径，用于检测"已打开后用不同路径再次 Open"
    // 这种 silent miss-switch 场景。空字符串 = 未打开。
    std::string devicePath_;
};


#endif // CABE_ENGINE_H
