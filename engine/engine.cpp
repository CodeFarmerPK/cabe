/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:31
 * Created by: CodeFarmerPK
 */

#include "engine.h"

#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"
#include <algorithm>
#include <cstring>

Engine::~Engine() {
    // Close() 在 unique_lock 内检查 isOpen_，避免析构函数裸读 isOpen_
    // 被 TSAN 标记为数据竞争（析构时要求调用方已无其他线程在使用 Engine）
    Close();
}

int32_t Engine::Open(const std::string& devicePath, const uint32_t bufferPoolCount) {
    std::unique_lock lock(mutex_);
    // 拒绝"Close 后在同实例上再 Open":metaIndex / chunkIndex / freeList /
    // nextChunkId 不会被 Close 重置,复用实例会让内存索引和新设备内容错位
    // (静默 corruption)。调用方必须销毁此实例构造新实例。
    // 本守卫必须在 isOpen_ 分支**之前**,否则 Close 后 isOpen_=false 会直接
    // 跳过幂等分支进入 fresh open 路径,守卫就形同虚设 —— 这正是之前 linter
    // 把本守卫删掉后 OpenAfterCloseRejected 测试失败的根因。请勿删除。
    if (usedOnce_) {
        return ENGINE_INSTANCE_USED;
    }

    if (isOpen_) {
        // 幂等分支:path 和 bufferPoolCount 都一致才算"同一次 Open 的重复调用"。
        // 任一不同都拒绝——静默忽略新参数(尤其是 pool size)会让调用方误以为
        // 自己扩/缩了 buffer pool,实际仍是首次 Open 时的尺寸。
        if (devicePath == devicePath_ && bufferPoolCount == bufferPoolCount_) {
            return SUCCESS;
        }
        return ENGINE_ALREADY_OPEN;
    }

    const int32_t ret = storage_.Open(devicePath);
    if (ret != SUCCESS) {
        return ret;
    }

    // --- 配置 FreeList block 数量上限(裸设备语义关键步骤) ---
    //
    // Storage::Open 已经在内部完成"字节 → block 数"翻译:用 ioctl(BLKGETSIZE64)
    // 取设备字节数,向下取整除以 CABE_VALUE_DATA_SIZE,把结果作为 blockCount_
    // 保存。此处直接拿 BlockCount() 即可,不需要再做 / CABE_VALUE_DATA_SIZE 算式。
    //
    // BlockCount() 由 Storage 保证 >= 1(否则 Open 阶段就返回 DEVICE_TOO_SMALL),
    // 所以这里不再需要 == 0 的兜底判断。
    freeList_.SetMaxBlockCount(storage_.BlockCount());

    // 初始化缓冲池：预分配 bufferPoolCount 个 1 MiB 对齐缓冲区
    const int32_t poolRet = bufferPool_.Init(CABE_VALUE_DATA_SIZE, bufferPoolCount);

    if (poolRet != SUCCESS) {
        storage_.Close();
        return poolRet;
    }

    // string 赋值在 OOM 下可能抛 bad_alloc(短路径走 SSO 不抛,长路径触发堆分配)。
    // 必须包 try/catch:
    //   1. ::Engine::Open 的契约是返回 int32_t 错误码,不能让异常穿透到 cabe::Engine::Open
    //   2. 异常穿透会破坏 storage_ / bufferPool_ 已成功初始化的资源:fd 不会被关、
    //      mmap 不会被 munmap → 资源泄漏(裸设备语义下 Engine 不再负责 backing 路径
    //      的清理,但 Engine 自身资源仍需 RAII 反向回滚)
    //   3. Storage::Open 已对同类 devicePath_ 赋值做了同样防护,此处补齐对称性
    try {
        devicePath_ = devicePath;
    } catch (...) {
        bufferPool_.Destroy();
        storage_.Close();
        return MEMORY_INSERT_FAIL;
    }
    bufferPoolCount_ = bufferPoolCount;
    isOpen_ = true;
    return SUCCESS;
}

int32_t Engine::Close() {
    std::unique_lock lock(mutex_);
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
    bufferPoolCount_ = 0;
    isOpen_ = false;
    // 标记此实例已走过一次 Open→Close,Open 检测到 usedOnce_ 即拒绝。
    // 即使 storage_.Close 失败(极罕见 EIO),此实例也不可复用 —— 避免半 closed
    // 状态的 Engine 被重开引入脏索引。
    usedOnce_ = true;
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
    std::unique_lock lock(mutex_);
    if (!isOpen_) {
        return ENGINE_NOT_OPEN;
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
    //
    // 这一段是 best-effort：任何一步失败只会让旧资源泄漏，不影响新 entry 的
    // 正确性，因此不再返回错误。但要保证 freeList 与 chunkIndex 状态一致：
    // 必须 ReleaseBatch 成功后才 RemoveRange。否则 blocks 既不在 freeList
    // 也无 chunkIndex 引用 → 永久泄漏（无法被任何 GC 路径回收）。
    if (hasOld) {
        std::vector<ChunkMeta> oldChunks;
        if (chunkIndex_.GetRange(oldMeta.firstChunkId, oldMeta.chunkCount, &oldChunks) == SUCCESS) {
            std::vector<BlockId> oldBlocks;
            try {
                oldBlocks.reserve(oldChunks.size());
            } catch (...) {
                // reserve 抛 bad_alloc：旧 chunkIndex 条目和 blocks 都保持原样
                // （metaIndex 已经覆盖，旧 chunks 不可达，但至少状态自洽）
                CABE_LOG_ERROR("Put: oldBlocks.reserve OOM, %u old chunks leaked", oldMeta.chunkCount);
                return SUCCESS;
            }
            for (const auto& cm : oldChunks) {
                oldBlocks.push_back(cm.blockId); // BlockId trivially-copyable, noexcept
            }

            const int32_t releaseRc = freeList_.ReleaseBatch(oldBlocks);
            if (releaseRc == SUCCESS) {
                // ReleaseBatch 已通过两层验证（无 batch 内/外重复），RemoveRange
                // 此处仅删除 oldMeta 范围内的条目，前面 GetRange 验证过完整性。
                (void) chunkIndex_.RemoveRange(oldMeta.firstChunkId, oldMeta.chunkCount);
            } else {
                // ReleaseBatch 失败：保留 chunkIndex 条目作为 "ghost"，至少
                // blockId 仍可从 chunkIndex 反向恢复（人工运维或未来 GC 工具）。
                // 不调用 RemoveRange，避免双重丢失。
                CABE_LOG_ERROR("Put: ReleaseBatch failed (code=%d), %u old chunks kept "
                               "as ghosts in chunkIndex (blockIds recoverable)",
                    releaseRc, oldMeta.chunkCount);
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
    //
    // 这些 chunk 都是当前 Put 在 unique_lock 保护下刚 chunkIndex_.Put(state=Active)
    // 成功的；持锁期间不可能被改成 Deleted（外部代码无法越过 mutex_ 改 ChunkMeta）。
    // 因此 Get 必然返回 SUCCESS——历史代码里的 `gr == CHUNK_DELETED` 防御分支
    // 在 P2 路径下是死代码，已删除。
    for (uint32_t j = 0; j < writtenCount; ++j) {
        ChunkMeta written{};
        if (chunkIndex_.Get(firstChunkId + j, &written) == SUCCESS) {
            (void)freeList_.Release(written.blockId);
        }
        (void)chunkIndex_.Remove(firstChunkId + j);
    }
    return failRet;
}

// ============================================================
// Get: 通过两层索引定位并合并所有 chunk
// ============================================================
int32_t Engine::Get(const std::string& key, DataBuffer buffer, uint64_t* readSize) const {
    // 契约：任何非 SUCCESS 返回前，*readSize 都会被置零。
    // readSize 必须最先检查，之后立即置零，确保后续所有早返回路径都满足契约。
    // 这两步不涉及 Engine 状态，在加锁前完成，避免空指针解引用持锁。
    if (readSize == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }
    *readSize = 0;
    std::shared_lock lock(mutex_);
    if (!isOpen_) {
        return ENGINE_NOT_OPEN;
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
// GetIntoVector: 读取数据到 vector（P2 API 层专用）
//
// 与 Get() 的核心逻辑相同，差异点：
//   1. 自行从 MetaIndex 取 totalSize，在单次 shared_lock 内 resize + 填充 vector，
//      消除"先查 size 再调 Get"两次加锁之间的 TOCTOU 竞态。
//   2. 输出类型为 std::vector<std::byte>（公开 API 的二进制语义），
//      memcpy 从 char* 到 std::byte* 合法（两者均为单字节类型）。
//   3. 失败时 *out 被清空，调用方无需额外清理。
// ============================================================
int32_t Engine::GetIntoVector(const std::string& key, std::vector<std::byte>* out) const {
    if (out == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }
    // 契约：除 nullptr 外的任何失败路径都 clear *out，避免调用方拿到 stale data。
    //
    // 前置 clear 覆盖 resize 之前的所有 early-return（!isOpen_ / key.empty /
    // metaIndex.Get 失败）；resize **之后**的失败路径（GetRange / ReadBlock /
    // CRC mismatch / pool exhausted）必须各自再 clear，因为此时 *out 已被
    // resize 到 totalSize 且可能部分填充，下方 4 处 out->clear() 就是为此保留。
    out->clear();
    std::shared_lock lock(mutex_);
    if (!isOpen_) {
        return ENGINE_NOT_OPEN;
    }
    if (key.empty()) {
        return CABE_EMPTY_KEY;
    }

    KeyMeta keyMeta{};
    int32_t ret = metaIndex_.Get(key, &keyMeta);
    if (ret != SUCCESS) {
        return ret;
    }

    try {
        out->resize(keyMeta.totalSize);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }

    std::vector<ChunkMeta> chunkMetas;
    ret = chunkIndex_.GetRange(keyMeta.firstChunkId, keyMeta.chunkCount, &chunkMetas);
    if (ret != SUCCESS) {
        out->clear();
        return ret;
    }

    for (uint32_t i = 0; i < keyMeta.chunkCount; ++i) {
        const ChunkMeta& meta = chunkMetas[i];

        char* alignedBuffer = bufferPool_.Acquire();
        if (alignedBuffer == nullptr) {
            out->clear();
            return POOL_EXHAUSTED;
        }

        ret = storage_.ReadBlock(meta.blockId, {alignedBuffer, CABE_VALUE_DATA_SIZE});
        if (ret != SUCCESS) {
            bufferPool_.Release(alignedBuffer);
            out->clear();
            return ret;
        }

        const uint64_t offset = static_cast<uint64_t>(i) * CABE_VALUE_DATA_SIZE;
        const uint64_t remaining = keyMeta.totalSize - offset;
        const uint64_t chunkSize = std::min(static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), remaining);

        // CRC 校验：与 Put 侧契约一致，只覆盖 [0, chunkSize) 的有效数据
        if (const uint32_t crc = cabe::util::CRC32({alignedBuffer, chunkSize}); crc != meta.crc) {
            bufferPool_.Release(alignedBuffer);
            out->clear();
            return DATA_CRC_MISMATCH;
        }

        // char* → std::byte*：两者均为单字节类型，memcpy 合法无 UB
        std::memcpy(out->data() + offset, alignedBuffer, chunkSize);
        bufferPool_.Release(alignedBuffer);
    }

    return SUCCESS;
}

// ============================================================
// Delete: 标记删除（两层都标记）
// ============================================================
int32_t Engine::Delete(const std::string& key) {
    std::unique_lock lock(mutex_);
    if (!isOpen_) {
        return ENGINE_NOT_OPEN;
    }

    if (key.empty()) {
        return CABE_EMPTY_KEY;
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
    std::unique_lock lock(mutex_);
    if (!isOpen_) {
        return ENGINE_NOT_OPEN;
    }

    if (key.empty()) {
        return CABE_EMPTY_KEY;
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
    std::shared_lock lock(mutex_);
    return metaIndex_.Size();
}

bool Engine::IsOpen() const {
    std::shared_lock lock(mutex_);
    return isOpen_;
}

ChunkId Engine::AllocateChunkIds(const uint32_t count) {
    // fetch_add 原子操作；调用方已持 unique_lock，但 atomic 保留此语义
    // 供未来 P3 并行写入时在无锁路径下分配 chunkId 使用。
    return nextChunkId_.fetch_add(count, std::memory_order_relaxed);
}
