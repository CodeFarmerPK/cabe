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
#include <utility>

Engine::~Engine() {
    // Close() 在 unique_lock 内检查 isOpen_，避免析构函数裸读 isOpen_
    // 被 TSAN 标记为数据竞争（析构时要求调用方已无其他线程在使用 Engine）
    Close();
}

int32_t Engine::Open(const std::string& devicePath, const uint32_t bufferPoolCount, const uint32_t sqDepth) {
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
        // 幂等分支:path、bufferPoolCount、sqDepth 全部一致才算"同一次 Open
        // 的重复调用"。任一不同都拒绝——静默忽略新参数(尤其是 pool size /
        // sq_depth)会让调用方误以为自己改了配置,实际仍是首次 Open 时的值。
        // P4 M6 加 sqDepth 到幂等校验(sync 后端虽然忽略 sqDepth,但仍参与
        // 校验保持 Engine 行为对两类后端一致)。
        if (devicePath == devicePath_
            && bufferPoolCount == bufferPoolCount_
            && sqDepth == sqDepth_) {
            return SUCCESS;
        }
        return ENGINE_ALREADY_OPEN;
    }

    // P3 M3:统一通过 IoBackend.Open 一步完成原 Storage::Open + BufferPool::Init。
    // io_ 内部按顺序处理:
    //   stat + S_ISBLK + open(O_DIRECT|O_SYNC) + ioctl(BLKGETSIZE64)
    //   → mmap pool + freeStack 倒序填充
    //   → io_uring 后端:queue_init + register_buffers + register_files
    // 任一阶段失败,io_ 内部已自行回滚(close fd / munmap),返回原 DEVICE_*/POOL_*
    // 错误码,Engine 直接透传即可,不必再做手动清理。
    //
    // P4 M6 起 sqDepth 透传到 io_.Open;sync 后端忽略,io_uring 后端用作
    // io_uring_queue_init 的 entries 并做 D7/R12 校验。
    const int32_t ret = io_.Open(devicePath, bufferPoolCount, sqDepth);
    if (ret != SUCCESS) {
        return ret;
    }

    // --- 配置 FreeList block 数量上限(裸设备语义关键步骤) ---
    //
    // IoBackend::Open 已经在内部完成"字节 → block 数"翻译(原 Storage 的逻辑迁入),
    // 此处直接拿 BlockCount() 即可,不需要再做 / CABE_VALUE_DATA_SIZE 算式。
    //
    // BlockCount() 由 IoBackend 保证 >= 1(否则 Open 阶段就返回 DEVICE_TOO_SMALL),
    // 所以这里不再需要 == 0 的兜底判断。
    freeList_.SetMaxBlockCount(io_.BlockCount());


    // string 赋值在 OOM 下可能抛 bad_alloc(短路径走 SSO 不抛,长路径触发堆分配)。
    // 必须包 try/catch:
    //   1. ::Engine::Open 的契约是返回 int32_t 错误码,不能让异常穿透到 cabe::Engine::Open
    //   2. 异常穿透会破坏 io_ 已成功初始化的资源(fd / mmap),需要 RAII 反向回滚
    try {
        devicePath_ = devicePath;
    } catch (...) {
        io_.Close();    // 一步释放 fd + munmap pool
        return MEMORY_INSERT_FAIL;
    }
    bufferPoolCount_ = bufferPoolCount;
    sqDepth_         = sqDepth;          // P4 M6:记录用于 re-Open 幂等校验
    isOpen_ = true;
    return SUCCESS;
}

int32_t Engine::Close() {
    std::unique_lock lock(mutex_);
    if (!isOpen_) {
        return SUCCESS;
    }

    // 无论 io_.Close 成功与否，最终都把状态机推回 "closed"。
    // 否则 close 极罕见失败时，Engine 会卡在 "isOpen_=true 但 io_ 已半释放"
    // 的中间态，用户看 IsOpen() 返回 true 却根本不能用。
    // io_.Close 内部对 munmap + close fd 都做了独立处理,部分失败也会
    // 把成员置零并设 closed_=true,与 Engine 这里的语义一致。
    const int32_t ioRc = io_.Close();

    devicePath_.clear();
    bufferPoolCount_ = 0;
    sqDepth_         = 0;     // P4 M6:同 bufferPoolCount_ 清零
    isOpen_ = false;
    // 标记此实例已走过一次 Open→Close,Open 检测到 usedOnce_ 即拒绝。
    // 即使 io_.Close 失败(极罕见 EIO),此实例也不可复用 —— 避免半 closed
    // 状态的 Engine 被重开引入脏索引。
    usedOnce_ = true;
    // 把 io_ 的真实失败码(底层 close/munmap 失败)透传给调用方
    return ioRc;
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

    // writtenCount = 已 chunkIndex.Put 落地的 chunk 总数(跨批累加)。
    // 失败时 rollback 路径按 [0, writtenCount) 逐个 chunkIndex.Remove + freeList.Release。
    // 批量路径细节:每批 chunkIndex.Put 全部成功后整批 +N;部分 j 个成功就 +j;
    // 已 Allocate 但还未 chunkIndex.Put 的 blocks 由内层显式 freeList.Release
    // (slots 析构归还 handle 到 buffer pool)。
    uint32_t writtenCount = 0;
    int32_t failRet = SUCCESS;

    // ----------------------------------------------------------------
    // P4 M7:批量 I/O 集成 —— 按 bufferPoolCount_ 分批。
    //   原 P3 路径每个 chunk 单独 Acquire → memcpy → WriteBlock → chunkIndex.Put,
    //   现在改成:
    //     Phase A 一批内 Acquire N 个 handle + fill + 补尾 + CRC
    //     Phase B 调一次 io_.WriteBlocks(span N)
    //             - io_uring 后端:一次 submit_and_wait(N) + 一次 sweep,
    //               省 (N-1) 次 syscall 来回
    //             - sync 后端:for-loop 等价于 N 次单笔 WriteBlock
    //     Phase C 写齐后逐 chunk chunkIndex.Put
    //   slots / writeBatch 提到循环外 reserve(bufferPoolCount_),
    //   每批 .clear() 复用 capacity,避免重分配。
    //
    //   batchN 上限 = min(remaining, bufferPoolCount_):buffer pool slot
    //   总数即一次能 in-flight 的 I/O 上限;io_uring 后端 R12 已校验
    //   sq_depth >= pool_count,所以 WriteBlocks 内 submit_and_wait(N)
    //   不会撞 SQ 满。
    // ----------------------------------------------------------------
    struct PutSlot {
        BlockId                blockId;
        cabe::io::BufferHandle handle;
        uint32_t               crc;
    };
    std::vector<PutSlot>                                                  slots;
    std::vector<std::pair<BlockId, const cabe::io::BufferHandle*>>        writeBatch;
    try {
        slots.reserve(bufferPoolCount_);
        writeBatch.reserve(bufferPoolCount_);
    } catch (...) {
        failRet = MEMORY_INSERT_FAIL;
        goto rollback;
    }

    for (uint32_t i = 0; i < chunkCount; ) {
        const uint32_t batchN = std::min<uint32_t>(chunkCount - i, bufferPoolCount_);
        slots.clear();
        writeBatch.clear();


        // ----- Phase A:批内 Acquire + memcpy + 补尾零 + CRC -----
        bool phaseAOk = true;
        for (uint32_t j = 0; j < batchN; ++j) {
            const uint64_t offset    = static_cast<uint64_t>(i + j) * CABE_VALUE_DATA_SIZE;
            const uint64_t remaining = totalSize - offset;
            const uint64_t chunkSize = std::min(
                static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), remaining);

            BlockId blockId;
            int32_t ret = freeList_.Allocate(&blockId);
            if (ret != SUCCESS) {
                // 本批已 Allocate 但还未 WriteBlocks 的 blocks → 显式 Release
                // (slots 析构会归还 handles 到 buffer pool)。
                for (auto& s : slots) freeList_.Release(s.blockId);
                failRet  = ret;
                phaseAOk = false;
                break;
            }
            cabe::io::BufferHandle h = io_.AcquireBuffer();
            if (!h.valid()) {
                freeList_.Release(blockId);
                for (auto& s : slots) freeList_.Release(s.blockId);
                failRet  = POOL_EXHAUSTED;
                phaseAOk = false;
                break;
            }
            std::memcpy(h.data(), data.data() + offset, chunkSize);

            // Q2 契约:AcquireBuffer 不再 memset(P3 M2 起 SyncIoBackend
            // 取消清零),小 chunk 必须显式补尾零 —— buffer 尾部残留会被
            // 后续 WriteBlocks 写到磁盘,破坏"同输入产同磁盘内容"契约。
            // CRC 只覆盖 [0, chunkSize),Get 侧只读 totalSize 字节,
            // 补零不影响读出正确性,但保证磁盘内容确定性。
            if (chunkSize < CABE_VALUE_DATA_SIZE) {
                std::memset(h.data() + chunkSize, 0,
                            CABE_VALUE_DATA_SIZE - chunkSize);
            }
            const uint32_t crc = cabe::util::CRC32({h.data(), chunkSize});


            // capacity 已 reserve(bufferPoolCount_),push 不再分配;
            // PutSlot 含 BufferHandle 是 move-only,push_back 触发 move。
            slots.push_back({blockId, std::move(h), crc});
        }
        if (!phaseAOk) {
            goto rollback;
        }


        // ----- Phase B:一次 io_.WriteBlocks(N 项)----
        // io_uring 后端把 N 个 SQE 一次提交并等齐 N 个 CQE,sync 后端
        // for-loop 等价于 N 次单笔 WriteBlock。失败语义:任一项失败整批
        // 失败,首个非 SUCCESS 错码透传;已写出到设备的 block 不回滚,
        // 但本批的 N 个 blocks(chunkIndex 中尚未登记)在这里显式 Release。
        for (const auto& s : slots) {
            writeBatch.emplace_back(s.blockId, &s.handle);
        }
        const int32_t wr = io_.WriteBlocks(writeBatch);
        if (wr != SUCCESS) {
            for (auto& s : slots) freeList_.Release(s.blockId);
            failRet = wr;
            goto rollback;
        }

        // ----- Phase C:批内逐 chunk chunkIndex.Put -----
        // chunkIndex.Put 几乎不失败(map insert,典型只在 bad_alloc 时失败)。
        // 一旦中途 j 失败:本批前 j 个已落 chunkIndex → writtenCount += j
        // 让外层 rollback 清理;后 (N-j) 个 blocks 未进 chunkIndex,
        // 这里显式 freeList.Release。
        for (uint32_t j = 0; j < slots.size(); ++j) {
            const ChunkMeta cm = {.blockId   = slots[j].blockId,
                                  .crc       = slots[j].crc,
                                  .timestamp = now,
                                  .state     = DataState::Active};
            const int32_t cr = chunkIndex_.Put(firstChunkId + i + j, cm);
            if (cr != SUCCESS) {
                for (uint32_t k = j; k < slots.size(); ++k) {
                    freeList_.Release(slots[k].blockId);
                }
                writtenCount += j;
                failRet = cr;
                goto rollback;
            }
        }
        writtenCount += static_cast<uint32_t>(slots.size());
        i += batchN;
        // slots 在下批 .clear() 时析构 PutSlot → BufferHandle dtor
        // → backend.ReturnBuffer_Internal,本批的 N 个 handle 在下批
        // Acquire 之前已经全部归还到 buffer pool。
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

    // ----------------------------------------------------------------
    // P4 M7:批量读 —— 按 bufferPoolCount_ 分批 Acquire N 个 handle,
    //   一次 io_.ReadBlocks(span)→ io_uring 后端 submit_and_wait(N) +
    //   一次 sweep CQE,sync 后端 for-loop 等价。然后批内逐 chunk CRC
    //   校验 + memcpy 到调用方 buffer。
    //
    //   GetSlot.meta 指向 chunkMetas[i+j],chunkMetas 在 GetRange 后
    //   不再修改,指针在 Get 返回前保持有效。
    // ----------------------------------------------------------------
    struct GetSlot {
        cabe::io::BufferHandle handle;
        const ChunkMeta*       meta;
        uint64_t               offset;
        uint64_t               chunkSize;
    };
    std::vector<GetSlot>                                              slots;
    std::vector<std::pair<BlockId, cabe::io::BufferHandle*>>          readBatch;
    try {
        slots.reserve(bufferPoolCount_);
        readBatch.reserve(bufferPoolCount_);
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }

    for (uint32_t i = 0; i < keyMeta.chunkCount; ) {
        const uint32_t batchN = std::min<uint32_t>(keyMeta.chunkCount - i,
                                                    bufferPoolCount_);
        slots.clear();
        readBatch.clear();

        // ----- Phase A:批内 Acquire N 个 handle + 记录 offset/chunkSize -----
        for (uint32_t j = 0; j < batchN; ++j) {
            cabe::io::BufferHandle h = io_.AcquireBuffer();
            if (!h.valid()) {
                // slots 内已 Acquire 的 handle 走出作用域 RAII 归还。
                return POOL_EXHAUSTED;
            }
            const uint64_t offset    = static_cast<uint64_t>(i + j) * CABE_VALUE_DATA_SIZE;
            const uint64_t remaining = keyMeta.totalSize - offset;
            const uint64_t chunkSize = std::min(
                static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), remaining);
            slots.push_back({std::move(h), &chunkMetas[i + j], offset, chunkSize});
        }

        // ----- Phase B:一次 io_.ReadBlocks(N 项)----
        // io_uring 后端把 N 个 read SQE 一次提交 + 等齐 N CQE;
        // sync 后端 for-loop pread 等价于 N 次单笔 ReadBlock。
        // 任一失败整批失败,首个非 SUCCESS 错码透传(slots dtor 自动归还)。
        for (auto& s : slots) {
            readBatch.emplace_back(s.meta->blockId, &s.handle);
        }
        ret = io_.ReadBlocks(readBatch);
        if (ret != SUCCESS) {
            return ret;
        }

        // ----- Phase C:批内逐 chunk CRC + memcpy 到调用方 buffer -----
        // CRC 与 Put 侧契约一致,只覆盖 [0, chunkSize) 的有效数据
        // (末 chunk 的 padding zero 不参与 CRC)。
        for (const auto& s : slots) {
            if (const uint32_t crc = cabe::util::CRC32({s.handle.data(), s.chunkSize});
                crc != s.meta->crc) {
                return DATA_CRC_MISMATCH;
            }
            std::memcpy(buffer.data() + s.offset, s.handle.data(), s.chunkSize);
        }

        i += batchN;
        // slots 在下批 .clear() 时归还本批所有 handle 到 buffer pool。
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

    // ----------------------------------------------------------------
    // P4 M7:批量读 —— 结构与 Get 完全对称,差异是写入 *out 而非 DataBuffer,
    //   失败路径需要 out->clear()(契约:除 nullptr 外的失败都 clear)。
    // ----------------------------------------------------------------
    struct GetSlot {
        cabe::io::BufferHandle handle;
        const ChunkMeta*       meta;
        uint64_t               offset;
        uint64_t               chunkSize;
    };
    std::vector<GetSlot>                                              slots;
    std::vector<std::pair<BlockId, cabe::io::BufferHandle*>>          readBatch;
    try {
        slots.reserve(bufferPoolCount_);
        readBatch.reserve(bufferPoolCount_);
    } catch (...) {
        out->clear();
        return MEMORY_INSERT_FAIL;
    }

    for (uint32_t i = 0; i < keyMeta.chunkCount; ) {
        const uint32_t batchN = std::min<uint32_t>(keyMeta.chunkCount - i,
                                                    bufferPoolCount_);
        slots.clear();
        readBatch.clear();

        // ----- Phase A:批内 Acquire -----
        for (uint32_t j = 0; j < batchN; ++j) {
            cabe::io::BufferHandle h = io_.AcquireBuffer();
            if (!h.valid()) {
                out->clear();
                return POOL_EXHAUSTED;
            }
            const uint64_t offset    = static_cast<uint64_t>(i + j) * CABE_VALUE_DATA_SIZE;
            const uint64_t remaining = keyMeta.totalSize - offset;
            const uint64_t chunkSize = std::min(
                static_cast<uint64_t>(CABE_VALUE_DATA_SIZE), remaining);
            slots.push_back({std::move(h), &chunkMetas[i + j], offset, chunkSize});
        }

        // ----- Phase B:一次 io_.ReadBlocks -----
        for (auto& s : slots) {
            readBatch.emplace_back(s.meta->blockId, &s.handle);
        }
        ret = io_.ReadBlocks(readBatch);
        if (ret != SUCCESS) {
            out->clear();
            return ret;
        }

        // ----- Phase C:批内 CRC + memcpy 到 *out -----
        // char* → std::byte*:两者都是单字节类型,memcpy 合法无 UB。
        for (const auto& s : slots) {
            if (const uint32_t crc = cabe::util::CRC32({s.handle.data(), s.chunkSize});
                crc != s.meta->crc) {
                out->clear();
                return DATA_CRC_MISMATCH;
            }
            std::memcpy(out->data() + s.offset, s.handle.data(), s.chunkSize);
        }
        i += batchN;
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
