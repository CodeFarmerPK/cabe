/*
 * Project: Cabe
 * Created Time: 2026-04-01 16:23
 * Created by: CodeFarmerPK
 */

#include "chunk_index.h"

int32_t ChunkIndex::Put(const ChunkId chunkId, const ChunkMeta& meta) {
    // insert_or_assign 避免 operator[] 的 "先默认构造再赋值" 两步。
    // ChunkMeta 是 trivial,性能差异可忽略;主要是语义更清晰,也少一次
    // 中间的默认构造对象存在于 map 里的中间态。
    try {
        chunkMap_.insert_or_assign(chunkId, meta);
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

    // 契约：失败时 *metas 保持调用前的状态，不留下"半填充"的脏数据；
    //       且一律返回 int32_t 错误码，不把异常泄到调用方。
    // 实现：单遍扫到局部 tmp，成功后 swap 进 *metas。
    //   - reserve 抛 bad_alloc 被 try/catch 转成 MEMORY_INSERT_FAIL
    //   - 检测到缺口直接 CHUNK_NOT_FOUND，*metas 原样（尚未被触碰）
    //   - ChunkMeta 是 trivial，reserve 之后的 push_back 是 noexcept
    //   - swap 本身 noexcept，且把原来的两次 lower_bound 合成一次
    std::vector<ChunkMeta> tmp;
    try {
        tmp.reserve(count);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }

    auto it = chunkMap_.lower_bound(firstChunkId);
    for (uint32_t i = 0; i < count; ++i) {
        if (it == chunkMap_.end() || it->first != firstChunkId + i) {
            return CHUNK_NOT_FOUND;
        }
        tmp.push_back(it->second);
        ++it;
    }

    metas->swap(tmp);
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
    // 两遍扫：先验证所有 chunk 存在，再批量 mutation，避免中途失败
    // 留下"一半改了一半没改"的部分失败状态。
    auto it = chunkMap_.lower_bound(firstChunkId);
    for (uint32_t i = 0; i < count; ++i) {
        if (it == chunkMap_.end() || it->first != firstChunkId + i) {
            return CHUNK_NOT_FOUND;
        }
        ++it;
    }

    // 第二遍 mutation，此时保证每一个 id 都命中
    it = chunkMap_.lower_bound(firstChunkId);
    for (uint32_t i = 0; i < count; ++i) {
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
    // 两遍扫：先验证再 erase，避免中途失败残留"前半被删、后半还在"
    auto it = chunkMap_.lower_bound(firstChunkId);
    for (uint32_t i = 0; i < count; ++i) {
        if (it == chunkMap_.end() || it->first != firstChunkId + i) {
            return CHUNK_NOT_FOUND;
        }
        ++it;
    }

    // 第二遍物理移除：从 lower_bound 重新定位起点，用 erase(it++) 线性推进
    // O(count)，与 DeleteRange 第二遍一致，避免每次 erase(key) 的 O(log N) 查找。
    it = chunkMap_.lower_bound(firstChunkId);
    for (uint32_t i = 0; i < count; ++i) {
        it = chunkMap_.erase(it);
    }
    return SUCCESS;
}

size_t ChunkIndex::Size() const {
    return chunkMap_.size();
}
