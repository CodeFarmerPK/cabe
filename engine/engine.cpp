/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:31
 * Created by: CodeFarmerPK
 */

#include "engine.h"

#include "util/crc32.h"
#include "util/util.h"
#include <algorithm>
#include <chrono>
#include <cstring>

// 缓冲池默认缓冲区数量
// 设为 8：为未来 io_uring 批量提交留出裕量
constexpr uint32_t DEFAULT_POOL_BUFFER_COUNT = 8;

Engine::~Engine() {
    if (isOpen_) {
        Close();
    }
}

int32_t Engine::Open(const std::string& devicePath) {
    if (isOpen_) {
        return SUCCESS;
    }

    const int32_t ret = storage_.Open(devicePath);
    if (ret != SUCCESS) {
        return ret;
    }

    // 初始化缓冲池：预分配 DEFAULT_POOL_BUFFER_COUNT 个 1MB 缓冲区
    const int32_t poolRet = bufferPool_.Init(CABE_VALUE_DATA_SIZE, DEFAULT_POOL_BUFFER_COUNT);
    if (poolRet != SUCCESS) {
        storage_.Close();
        return poolRet;
    }

    isOpen_ = true;
    return SUCCESS;
}

int32_t Engine::Close() {
    if (!isOpen_) {
        return SUCCESS;
    }

    // 先销毁缓冲池，再关闭存储
    bufferPool_.Destroy();

    int32_t ret = storage_.Close();
    if (ret != SUCCESS) {
        return ret;
    }

    isOpen_ = false;
    return SUCCESS;
}

// ============================================================
// Put: 将 data 拆分为多个 chunk 写入
// ============================================================
int32_t Engine::Put(const std::string& key, const DataView data) {
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    if (key.empty()) {
        return CABE_EMPTY_KEY;
    }
    if (data.empty()) {
        return CABE_EMPTY_VALUE;
    }

    const uint64_t totalSize = data.size();
    const auto chunkCount = static_cast<uint32_t>((totalSize + CABE_VALUE_DATA_SIZE - 1) / CABE_VALUE_DATA_SIZE);

    // 分配连续的 chunkId: [firstChunkId, firstChunkId + chunkCount)
    const ChunkId firstChunkId = AllocateChunkIds(chunkCount);
    const uint64_t now = cabe::util::GetTimeStamp();

    // 记录已成功写入的 chunk 数量，用于失败时回滚
    uint32_t writtenCount = 0;

    // 逐 chunk 写入
    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint64_t offset = static_cast<uint64_t>(i) * CABE_VALUE_DATA_SIZE;
        const uint64_t remaining = totalSize - offset;
        const uint64_t chunkSize = std::min(static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), remaining);

        // 分配磁盘块
        BlockId blockId;
        int32_t ret = freeList_.Allocate(&blockId);
        if (ret != SUCCESS) {
            goto rollback;
        }

        {
            // 分配对齐内存，拷贝数据
            char* alignedBuffer = bufferPool_.Acquire();
            if (alignedBuffer == nullptr) {
                freeList_.Release(blockId);
                goto rollback;
            }
            std::memcpy(alignedBuffer, data.data() + offset, chunkSize);

            // 写入磁盘
            ret = storage_.WriteBlock(blockId, {alignedBuffer, CABE_VALUE_DATA_SIZE});
            if (ret != SUCCESS) {
                bufferPool_.Release(alignedBuffer);
                freeList_.Release(blockId);
                goto rollback;
            }

            // 计算 CRC
            const uint32_t crc = cabe::util::CRC32({alignedBuffer, CABE_VALUE_DATA_SIZE});
            bufferPool_.Release(alignedBuffer);

            // 写入第二层索引
            const ChunkMeta chunkMeta = {.blockId = blockId, .crc = crc, .timestamp = now, .state = DataState::Active};
            ret = chunkIndex_.Put(firstChunkId + i, chunkMeta);
            if (ret != SUCCESS) {
                freeList_.Release(blockId);
                goto rollback;
            }
        }

        ++writtenCount;
    }

    {
        // 写入第一层索引
        const KeyMeta keyMeta = {.firstChunkId = firstChunkId,
            .chunkCount = chunkCount,
            .totalSize = totalSize,
            .createdAt = now,
            .modifiedAt = now,
            .state = DataState::Active};
        return metaIndex_.Put(key, keyMeta);
    }

rollback:
    // 回滚已成功写入的 0 ~ writtenCount-1 个 chunk
    for (uint32_t j = 0; j < writtenCount; ++j) {
        ChunkMeta written{};
        if (chunkIndex_.Get(firstChunkId + j, &written) == SUCCESS) {
            freeList_.Release(written.blockId);
        }
        chunkIndex_.Remove(firstChunkId + j);
    }
    return MEMORY_INSERT_FAIL;
}

// ============================================================
// Get: 通过两层索引定位并合并所有 chunk
// ============================================================
int32_t Engine::Get(const std::string& key, DataBuffer buffer, uint64_t* readSize) const {
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    if (key.empty()) {
        return CABE_EMPTY_KEY;
    }
    if (buffer.empty() || readSize == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }

    // 第一层: key → KeyMeta{firstChunkId, chunkCount, totalSize}
    KeyMeta keyMeta{};
    int32_t ret = metaIndex_.Get(key, &keyMeta);
    if (ret != SUCCESS) {
        return ret;
    }

    if (buffer.size() < keyMeta.totalSize) {
        return CABE_INVALID_DATA_SIZE;
    }

    // 第二层: 从 firstChunkId 顺序遍历 chunkCount 个 ChunkMeta
    std::vector<ChunkMeta> chunkMetas;
    ret = chunkIndex_.GetRange(keyMeta.firstChunkId, keyMeta.chunkCount, &chunkMetas);
    if (ret != SUCCESS) {
        return ret;
    }

    // 逐 chunk 读取并拼接
    for (uint32_t i = 0; i < keyMeta.chunkCount; ++i) {
        const ChunkMeta& meta = chunkMetas[i];

        char* alignedBuffer = bufferPool_.Acquire();
        if (alignedBuffer == nullptr) {
            return POOL_EXHAUSTED;
        }

        ret = storage_.ReadBlock(meta.blockId, {alignedBuffer, CABE_VALUE_DATA_SIZE});
        if (ret != SUCCESS) {
            bufferPool_.Release(alignedBuffer);
            return ret;
        }

        // CRC 校验
        if (const uint32_t crc = cabe::util::CRC32({alignedBuffer, CABE_VALUE_DATA_SIZE}); crc != meta.crc) {
            bufferPool_.Release(alignedBuffer);
            return DATA_CRC_MISMATCH;
        }

        // 拷贝到 buffer
        const uint64_t offset = static_cast<uint64_t>(i) * CABE_VALUE_DATA_SIZE;
        const uint64_t remaining = keyMeta.totalSize - offset;
        const uint64_t copySize = std::min(static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), remaining);

        std::memcpy(buffer.data() + offset, alignedBuffer, copySize);
        bufferPool_.Release(alignedBuffer);
    }

    *readSize = keyMeta.totalSize;
    return SUCCESS;
}

// ============================================================
// Delete: 标记删除（两层都标记）
// ============================================================
int32_t Engine::Delete(const std::string& key) {
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    // 获取 chunk 范围
    KeyMeta keyMeta{};
    int32_t ret = metaIndex_.Get(key, &keyMeta);
    if (ret != SUCCESS) {
        return ret;
    }

    // 标记第二层
    ret = chunkIndex_.DeleteRange(keyMeta.firstChunkId, keyMeta.chunkCount);
    if (ret != SUCCESS) {
        return ret;
    }

    // 标记第一层
    return metaIndex_.Delete(key);
}

// ============================================================
// Remove: 物理移除，回收磁盘块
// ============================================================
int32_t Engine::Remove(const std::string& key) {
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    // 获取 KeyMeta（需要能获取已删除条目的 chunk 范围）
    KeyMeta keyMeta{};
    int32_t ret = metaIndex_.Get(key, &keyMeta);
    if (ret != SUCCESS && ret != INDEX_KEY_DELETED) {
        return ret;
    }

    // 回收所有 chunk 对应的磁盘块
    for (uint32_t i = 0; i < keyMeta.chunkCount; ++i) {
        ChunkMeta chunkMeta{};
        int32_t getResult = chunkIndex_.Get(keyMeta.firstChunkId + i, &chunkMeta);
        if (getResult == SUCCESS || getResult == CHUNK_DELETED) {
            freeList_.Release(chunkMeta.blockId);
        }
    }

    // 物理移除第二层
    chunkIndex_.RemoveRange(keyMeta.firstChunkId, keyMeta.chunkCount);

    // 物理移除第一层
    return metaIndex_.Remove(key);
}

size_t Engine::Size() const {
    return metaIndex_.Size();
}

bool Engine::IsOpen() const {
    return isOpen_;
}

ChunkId Engine::AllocateChunkIds(const uint32_t count) {
    const ChunkId first = nextChunkId_;
    nextChunkId_ += count;
    return first;
}
