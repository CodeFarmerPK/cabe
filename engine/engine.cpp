/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:31
 * Created by: CodeFarmerPK
 */

#include "engine.h"

#include "util/crc32.h"
#include "util/util.h"
#include <algorithm>
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
        // 同路径再次 Open → 幂等返回 SUCCESS
        // 不同路径再次 Open → 拒绝，避免静默忽略导致用户以为切换成功
        // 想真正切换路径必须先 Close()
        if (devicePath == devicePath_) {
            return SUCCESS;
        }
        return DEVICE_FAILED_TO_OPEN_DEVICE;
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

    devicePath_ = devicePath;
    isOpen_ = true;
    return SUCCESS;
}

int32_t Engine::Close() {
    if (!isOpen_) {
        return SUCCESS;
    }

    // 无论各步成功与否，最终都把状态机推回 "closed"。
    // 否则 storage_.Close 极罕见情况下失败时，Engine 会卡在
    // "isOpen_=true 但 bufferPool 已 destroy / fd 已 -1" 的半状态，
    // 用户看 IsOpen() 返回 true 却根本不能用。
    // bufferPool_.Destroy 和 storage_.Close 都是幂等的，
    // 双重调用安全。
    bufferPool_.Destroy();
    const int32_t storageRc = storage_.Close();

    devicePath_.clear();
    isOpen_ = false;
    // 把 storage 的真实失败码透传给调用方
    return storageRc;
}

// ============================================================
// Put: 将 data 拆分为多个 chunk 写入
//
// 覆盖写语义：如果 key 已存在（含已 Delete 状态），新 Put 完成后
// 旧的 chunks/blocks 会被回收，避免线性资源泄漏。
//
// 失败语义：写入主流程任何一步失败都走 rollback，新分配的 blocks
// 和 chunkIndex 条目都会被释放；旧 entry 不动。
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
    // value 大小上限：chunkCount 必须在 uint32_t 范围内，防止下方 static_cast 截断。
    // 2^32 × 1 MiB = 4 PiB，对任何单 value 都远超合理范围。
    constexpr uint64_t kMaxValueSize = static_cast<uint64_t>(UINT32_MAX) * CABE_VALUE_DATA_SIZE;
    if (totalSize > kMaxValueSize) {
        return CABE_INVALID_DATA_SIZE;
    }
    const auto chunkCount = static_cast<uint32_t>((totalSize + CABE_VALUE_DATA_SIZE - 1) / CABE_VALUE_DATA_SIZE);


    // 先记录是否存在旧 entry，用于覆盖写完成后的清理。
    // 注意：INDEX_KEY_DELETED 也算"存在"——已 Delete 但未 Remove 的 key
    // 仍然占着 chunkIndex 条目和 blocks。
    KeyMeta oldMeta{};
    const int32_t oldRet = metaIndex_.Get(key, &oldMeta);
    const bool hasOld = (oldRet == SUCCESS || oldRet == INDEX_KEY_DELETED);

    // 分配连续的 chunkId: [firstChunkId, firstChunkId + chunkCount)
    const ChunkId firstChunkId = AllocateChunkIds(chunkCount);
    const uint64_t now = cabe::util::GetWallTimeNs();

    // 记录已成功写入的 chunk 数量，用于失败时回滚
    uint32_t writtenCount = 0;
    int32_t failRet = SUCCESS;

    // 逐 chunk 写入
    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint64_t offset = static_cast<uint64_t>(i) * CABE_VALUE_DATA_SIZE;
        const uint64_t remaining = totalSize - offset;
        const uint64_t chunkSize = std::min(static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), remaining);

        // 分配磁盘块
        BlockId blockId;
        int32_t ret = freeList_.Allocate(&blockId);
        if (ret != SUCCESS) {
            failRet = ret;
            goto rollback;
        }

        {
            // 分配对齐内存，拷贝数据
            char* alignedBuffer = bufferPool_.Acquire();
            if (alignedBuffer == nullptr) {
                freeList_.Release(blockId);
                failRet = POOL_EXHAUSTED;
                goto rollback;
            }
            std::memcpy(alignedBuffer, data.data() + offset, chunkSize);

            // 写入磁盘
            ret = storage_.WriteBlock(blockId, {alignedBuffer, CABE_VALUE_DATA_SIZE});
            if (ret != SUCCESS) {
                bufferPool_.Release(alignedBuffer);
                freeList_.Release(blockId);
                failRet = ret;
                goto rollback;
            }

            // CRC 只覆盖有效数据 [0, chunkSize)，不含末 chunk 的 padding zero。
            // ChunkMeta.crc 的契约：与 Get 侧用同样公式算出的 chunkSize 相匹配
            const uint32_t crc = cabe::util::CRC32({alignedBuffer, chunkSize});
            bufferPool_.Release(alignedBuffer);

            // 写入第二层索引
            const ChunkMeta chunkMeta = {.blockId = blockId, .crc = crc, .timestamp = now, .state = DataState::Active};
            ret = chunkIndex_.Put(firstChunkId + i, chunkMeta);
            if (ret != SUCCESS) {
                freeList_.Release(blockId);
                failRet = ret;
                goto rollback;
            }
        }

        ++writtenCount;
    }

    {
        // 写入第一层索引（覆盖或新建）
        const KeyMeta keyMeta = {.firstChunkId = firstChunkId,
            .chunkCount = chunkCount,
            .totalSize = totalSize,
            .createdAt = now,
            .modifiedAt = now,
            .state = DataState::Active};
        const int32_t metaRet = metaIndex_.Put(key, keyMeta);
        if (metaRet != SUCCESS) {
            // metaIndex.Put 失败（典型是 bad_alloc）：刚写好的所有
            // chunks + blocks 必须回滚，否则永久泄漏。复用 rollback 路径。
            failRet = metaRet;
            goto rollback;
        }
    }

    // 走到这里：新 entry 已落地，key 已指向新 KeyMeta。
    // 现在清理旧 entry 的 chunks + blocks（覆盖写场景）。
    // 这一段是 best-effort：任何一步失败只会让旧资源泄漏，不影响
    // 新 entry 的正确性，因此不再返回错误。
    if (hasOld) {
        std::vector<ChunkMeta> oldChunks;
        if (chunkIndex_.GetRange(oldMeta.firstChunkId, oldMeta.chunkCount, &oldChunks) == SUCCESS) {
            try {
                std::vector<BlockId> oldBlocks;
                oldBlocks.reserve(oldChunks.size());
                for (const auto& cm : oldChunks) {
                    oldBlocks.push_back(cm.blockId);
                }
                freeList_.ReleaseBatch(oldBlocks);
                chunkIndex_.RemoveRange(oldMeta.firstChunkId, oldMeta.chunkCount);
            } catch (...) {
                // OOM：旧 blocks 泄漏，但新 entry 已落地，不影响正确性
            }
        }
        // 若 GetRange 失败（chunkIndex 与 metaIndex 不一致），旧 chunks
        // 已不可达（metaIndex 已被覆盖），无法清理。属于前置不变式被
        // 破坏的极端场景，这里选择"宁可泄漏不破坏新 entry"。
    }

    return SUCCESS;

rollback:
    // 回滚已成功写入的 0 ~ writtenCount-1 个 chunk。
    // 新 metaIndex.Put 失败的场景下也走这里，保证新 chunks/blocks 被清。
    for (uint32_t j = 0; j < writtenCount; ++j) {
        ChunkMeta written{};
        if (const int32_t gr = chunkIndex_.Get(firstChunkId + j, &written); gr == SUCCESS || gr == CHUNK_DELETED) {
            freeList_.Release(written.blockId);
        }
        chunkIndex_.Remove(firstChunkId + j);
    }
    return failRet;
}

// ============================================================
// Get: 通过两层索引定位并合并所有 chunk
// ============================================================
int32_t Engine::Get(const std::string& key, DataBuffer buffer, uint64_t* readSize) const {
    // 契约：任何非 SUCCESS 返回前，*readSize 都会被置零。
    // readSize 必须最先检查，之后立即置零，确保后续所有早返回路径都满足契约。
    if (readSize == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }
    *readSize = 0;
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    if (key.empty()) {
        return CABE_EMPTY_KEY;
    }
    if (buffer.empty()) {
        return CABE_INVALID_DATA_SIZE;
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
        // 先算本 chunk 的有效字节数：末 chunk 可能小于 CABE_VALUE_DATA_SIZE
        const uint64_t offset = static_cast<uint64_t>(i) * CABE_VALUE_DATA_SIZE;
        const uint64_t remaining = keyMeta.totalSize - offset;
        const uint64_t chunkSize = std::min(static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), remaining);

        // CRC 校验：与 Put 侧契约一致，只覆盖 [0, chunkSize) 的有效数据
        if (const uint32_t crc = cabe::util::CRC32({alignedBuffer, chunkSize}); crc != meta.crc) {
            bufferPool_.Release(alignedBuffer);
            return DATA_CRC_MISMATCH;
        }

        // 拷贝到 buffer
        std::memcpy(buffer.data() + offset, alignedBuffer, chunkSize);
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
// 事务语义：要么完整回收（blocks + chunkIndex + metaIndex 都清），
// 要么完全不改任何状态。任一环节失败立即 early return，避免
// "blocks 已回收但 chunkIndex 还在"这种不一致。
int32_t Engine::Remove(const std::string& key) {
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    // 1. 获取 KeyMeta（允许已 Delete 过的 key，需要它的 chunk 范围）
    KeyMeta keyMeta{};
    int32_t ret = metaIndex_.Get(key, &keyMeta);
    if (ret != SUCCESS && ret != INDEX_KEY_DELETED) {
        return ret;
    }

    // 2. 原子拉取所有 ChunkMeta。GetRange 若发现缺口整体失败不 push，
    //    此时直接返回错误，底层状态未改动
    std::vector<ChunkMeta> metas;
    const int32_t rangeRet = chunkIndex_.GetRange(keyMeta.firstChunkId, keyMeta.chunkCount, &metas);
    if (rangeRet != SUCCESS) {
        // chunkIndex 与 metaIndex 已经不一致（正常路径不会出现，可能由
        // 未来的 race / 外部污染引起）。此处选择 early-fail，让调用方看到
        // 错误码 —— 不自作主张去修复，也不继续 mutation
        return rangeRet;
    }

    // 3. 先收集 blockId。此处 reserve 自身可能 bad_alloc，但仍在「只读」
    //    阶段——freeList / chunkIndex / metaIndex 均未动，直接返回错误
    //    即可保持事务语义（要么完整回收，要么完全不改）
    std::vector<BlockId> blocks;
    try {
        blocks.reserve(metas.size());
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    for (const auto& m : metas) {
        blocks.push_back(m.blockId);
    }

    // 4. 批量回收 block。ReleaseBatch 是原子的（内部 reserve 失败则整批不
    //    insert），返回非 SUCCESS 时 freeList 未变；此时**不能**继续往下
    //    走 RemoveRange，否则 chunkIndex 被清掉、blocks 却没进 freeList
    //    → 永久泄漏。返回错误由调用方决定重试，状态保持一致
    const int32_t releaseRet = freeList_.ReleaseBatch(blocks);
    if (releaseRet != SUCCESS) {
        return releaseRet;
    }

    // 5. 物理移除第二层。GetRange 刚验证过完整性，这里保证成功
    if (const int32_t rc = chunkIndex_.RemoveRange(keyMeta.firstChunkId, keyMeta.chunkCount); rc != SUCCESS) {
        return rc;
    }

    // 6. 物理移除第一层
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
