/*
 * Project: Cabe
 * Created Time: 2026-04-01 16:23
 * Created by: CodeFarmerPK
 */

#include "chunk_index.h"

int32_t ChunkIndex::Put(const ChunkId chunkId, const ChunkMeta& meta) {
    try {
        chunkMap_[chunkId] = meta;
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    return SUCCESS;
}

int32_t ChunkIndex::Get(const ChunkId chunkId, ChunkMeta* meta) const {
    if (meta == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }

    const auto it = chunkMap_.find(chunkId);
    if (it == chunkMap_.end()) {
        return CHUNK_NOT_FOUND;
    }

    *meta = it->second;

    if (it->second.state != DataState::Active) {
        return CHUNK_DELETED;
    }

    return SUCCESS;
}

int32_t ChunkIndex::GetRange(const ChunkId firstChunkId, const uint32_t count, std::vector<ChunkMeta>* metas) const {
    if (metas == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }

    metas->clear();
    metas->reserve(count);


    // lower_bound 一次定位，然后 iterator++ 顺序扫描
    // 等价于 B+ 树: 定位叶子节点后沿链表遍历，复杂度 O(log N + count)
    auto it = chunkMap_.lower_bound(firstChunkId);

    for (uint32_t i = 0; i < count; ++i) {
        if (it == chunkMap_.end() || it->first != firstChunkId + i) {
            return CHUNK_NOT_FOUND;
        }
        metas->push_back(it->second);
        ++it;
    }

    return SUCCESS;
}

int32_t ChunkIndex::Delete(const ChunkId chunkId) {
    const auto it = chunkMap_.find(chunkId);
    if (it == chunkMap_.end()) {
        return CHUNK_NOT_FOUND;
    }

    if (it->second.state == DataState::Deleted) {
        return CHUNK_DELETED;
    }

    it->second.state = DataState::Deleted;
    return SUCCESS;
}

int32_t ChunkIndex::DeleteRange(const ChunkId firstChunkId, const uint32_t count) {
    auto it = chunkMap_.lower_bound(firstChunkId);

    for (uint32_t i = 0; i < count; ++i) {
        if (it == chunkMap_.end() || it->first != firstChunkId + i) {
            return CHUNK_NOT_FOUND;
        }
        it->second.state = DataState::Deleted;
        ++it;
    }
    return SUCCESS;
}

int32_t ChunkIndex::Remove(const ChunkId chunkId) {
    if (const auto count = chunkMap_.erase(chunkId); count == 0) {
        return CHUNK_NOT_FOUND;
    }
    return SUCCESS;
}

int32_t ChunkIndex::RemoveRange(const ChunkId firstChunkId, const uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const ChunkId id = firstChunkId + i;
        if (const auto erased = chunkMap_.erase(id); erased == 0) {
            return CHUNK_NOT_FOUND;
        }
    }
    return SUCCESS;
}

size_t ChunkIndex::Size() const {
    return chunkMap_.size();
}
