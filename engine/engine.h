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

    // 初始化引擎
    int32_t Open(const std::string& devicePath);

    // 关闭引擎
    int32_t Close();

    // 写入数据（支持大文件，自动拆分为多个 chunk）
    int32_t Put(const std::string& key, DataView data);

    // 读取数据（自动合并多个 chunk）
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
};


#endif // CABE_ENGINE_H
