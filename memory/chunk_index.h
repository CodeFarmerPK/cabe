/*
 * Project: Cabe
 * Created Time: 2026-04-01 16:23
 * Created by: CodeFarmerPK
 */

#ifndef CABE_CHUNK_INDEX_H
#define CABE_CHUNK_INDEX_H

#include "common/error_code.h"
#include "common/structs.h"
#include <cstdint>
#include <map>
#include <vector>

class ChunkIndex {
public:
    ChunkIndex() = default;
    ~ChunkIndex() = default;

    // 插入单个 chunk
    int32_t Put(ChunkId chunkId, const ChunkMeta& meta);

    // 查询单个 chunk
    int32_t Get(ChunkId chunkId, ChunkMeta* meta) const;

    // 范围查询: 从 firstChunkId 开始连续获取 count 个 ChunkMeta
    int32_t GetRange(ChunkId firstChunkId, uint32_t count, std::vector<ChunkMeta>* metas) const;

    // 标记删除单个 chunk
    int32_t Delete(ChunkId chunkId);

    // 批量标记删除: 从 firstChunkId 开始连续 count 个
    int32_t DeleteRange(ChunkId firstChunkId, uint32_t count);

    // 物理移除单个 chunk
    int32_t Remove(ChunkId chunkId);

    // 批量物理移除
    int32_t RemoveRange(ChunkId firstChunkId, uint32_t count);

    size_t Size() const;

private:
    // 当前用 std::map 模拟 B+ 树的有序特性
    // std::map 天然支持 lower_bound / iterator++ 顺序遍历
    std::map<ChunkId, ChunkMeta> chunkMap_;
};


#endif // CABE_CHUNK_INDEX_H
