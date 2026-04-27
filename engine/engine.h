/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:31
 * Created by: CodeFarmerPK
 */

#ifndef CABE_ENGINE_H
#define CABE_ENGINE_H

#include "common/error_code.h"
#include "common/structs.h"
#include "io/io_backend.h"
#include "memory/chunk_index.h"
#include "memory/meta_index.h"
#include "storage/free_list.h"

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

    // Engine 持有 IoBackend(内部含 fd + mmap pool)等独占资源，语义上不允许
    // 拷贝或移动（否则会出现两个 Engine 共享同一 fd / mmap 区的 UB）。
    // 显式 = delete 让错误调用在编译期就被发现，而不是传递到成员层面。
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    // bufferPoolCount：IoBackend 内部 buffer 池中的 1 MiB 对齐缓冲区数量（默认 8）
    //
    // 生命周期约束(P3 前,代码层强制):
    //   1. 同实例 Open(P, N) → Open(P, N) 幂等 SUCCESS(path + pool 都一致)
    //   2. 同实例 Open(P1, N) → Open(P2, N) 返回 ENGINE_ALREADY_OPEN(path 不同)
    //      —— 避免用户以为路径切换成功但引擎仍指向旧设备
    //   3. 同实例 Open(P, N) → Open(P, M) 返回 ENGINE_ALREADY_OPEN(pool 不同)
    //      —— 避免新 pool 参数被静默忽略
    //   4. 同实例 Open → Close → Open 返回 ENGINE_INSTANCE_USED
    //      —— metaIndex_ / chunkIndex_ / freeList_ / nextChunkId_ 不会被 Close
    //      重置,在同实例上复用会和新设备内容静默 corruption。
    //      想"重新打开"必须销毁此实例构造新实例。
    //   cabe::Engine 公开 API 通过工厂方法 + unique_ptr 天然满足约束 4:
    //   每次 cabe::Engine::Open 都 new 一个新 Engine 实例,老实例在 unique_ptr
    //   重置时析构。
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
    // P4 引入 io_uring 后，Put 的 I/O 阶段将脱离锁保护，届时锁持有时间大幅缩短。
    // mutable：Get/Size/IsOpen 是 const 方法，但仍需获取 shared_lock。
    //
    // TODO(P4+ 多 NVMe)：当前 mutex_ 同时保护 metaIndex_/chunkIndex_/freeList_/io_。
    //   多 NVMe 路线（roadmap）需要：
    //     1. freeList_ / io_ 改为 std::vector<FreeList> / std::vector<cabe::io::IoBackend>，每设备一个
    //     2. 每个 FreeList / IoBackend 持自己的 mutex（去除对 Engine mutex 的依赖）
    //     3. ChunkId → DeviceIndex 的策略层加在 AllocateChunkIds 上
    //     4. mutex_ 缩小到只保护 metaIndex_ / chunkIndex_
    //   这样单 NVMe 性能不变，多 NVMe 时各设备的 I/O / FreeList 操作可真并行。
    mutable std::shared_mutex mutex_;
    MetaIndex metaIndex_; // 第一层: key(string) → KeyMeta{firstChunkId, chunkCount}
    ChunkIndex chunkIndex_; // 第二层: chunkId → ChunkMeta (std::map, 有序)
    FreeList freeList_; // 磁盘块号分配(裸设备 BlockId 上限)

    // P3 M3 起:io_ 合并了原 Storage + BufferPool 的职责 ——
    //   - 设备开关 / pread / pwrite(原 Storage)
    //   - 1 MiB 对齐 buffer 池 LIFO 分配 / 归还(原 BufferPool)
    //   - BufferHandle PIMPL 让 Engine 不必触碰底层切片或 fd
    //
    // mutable: Get() 是 const 方法,但需要 io_.AcquireBuffer 修改 backend 内部
    // 状态(LIFO 栈 / outstanding_count_)。语义上 Get 不改变引擎的「逻辑状态」,
    // pool 是 backend 内部实现细节。
    // IoBackend 内部用 atomic + poolMutex_ 保护并发,不依赖 Engine mutex。
    //
    // 编译期 dispatch:CABE_IO_BACKEND_SYNC=1 时 io_ 是 SyncIoBackend;P4 切到
    // io_uring 后会变为 IoUringIoBackend,Engine 代码无需修改(concept 保证签名一致)。
    mutable cabe::io::IoBackend io_;

    // fetch_add 原子自增，P3 并行写入时无需持 Engine 锁即可分配 chunkId
    std::atomic<ChunkId> nextChunkId_{0};
    bool isOpen_ = false;

    // 一次性使用标记:Close 成功后置为 true,后续在同实例上再 Open 直接拒绝。
    // 目的是拦下"Close → Open 同实例"这类静默 corruption 场景(内存索引
    // 残留与新设备内容不一致)。从未 Open 过的实例此值保持 false。
    bool usedOnce_ = false;

    // 记录 Open 时使用的路径,用于检测"已打开后用不同路径再次 Open"
    // 这种 silent miss-switch 场景。空字符串 = 未打开。
    std::string devicePath_;

    // 记录 Open 时使用的 bufferPoolCount,检测"同 path 但 pool 参数不同"
    // 的幂等伪装。0 = 未打开。
    uint32_t bufferPoolCount_ = 0;
};


#endif // CABE_ENGINE_H
