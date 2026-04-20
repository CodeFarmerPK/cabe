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
#include <cstdint>
#include <string>

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
    // 初始化引擎
    int32_t Open(const std::string& devicePath);

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


    MetaIndex metaIndex_; // 第一层: key(string) → KeyMeta{firstChunkId, chunkCount}
    ChunkIndex chunkIndex_; // 第二层: chunkId → ChunkMeta (std::map, 有序)
    FreeList freeList_; // 磁盘块分配
    Storage storage_; // 磁盘 I/O

    // mutable: Get() 是 const 方法，但需要 Acquire/Release 修改池内部状态
    // 语义正确：Get 不改变引擎的「逻辑状态」，池是内部实现细节
    mutable BufferPool bufferPool_;

    ChunkId nextChunkId_ = 0; // chunkId 全局自增
    bool isOpen_ = false;

    // 记录 Open 时使用的路径，用于检测"已打开后用不同路径再次 Open"
    // 这种 silent miss-switch 场景。空字符串 = 未打开。
    std::string devicePath_;
};


#endif // CABE_ENGINE_H
