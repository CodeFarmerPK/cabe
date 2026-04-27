/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:37
 * Created by: CodeFarmerPK
 */
#ifndef CABE_ERROR_CODE_H
#define CABE_ERROR_CODE_H

#include <cstdint>

#define SUCCESS 0

// memory codes
#define MEMORY_NULL_POINTER_EXCEPTION (-100000)
#define MEMORY_EMPTY_KEY              (-100001)
#define MEMORY_EMPTY_VALUE            (-100002)
#define MEMORY_INSERT_FAIL            (-100003)

// index codes
#define INDEX_KEY_NOT_FOUND (-200000)
#define INDEX_KEY_DELETED   (-200001)

#define CHUNK_NOT_FOUND     (-300000)
#define CHUNK_DELETED       (-300001)
#define CHUNK_RANGE_INVALID (-300002)

// device codes
#define DEVICE_FAILED_TO_OPEN_DEVICE  (-400000)
#define DEVICE_FAILED_TO_CLOSE_DEVICE (-400001)
#define DEVICE_FAILED_TO_SEEK_OFFSET  (-400002)
#define DEVICE_FAILED_TO_WRITE_DATA   (-400003)
#define DEVICE_FAILED_TO_READ_DATA    (-400004)
// 裸设备语义新增码:
//   NOT_BLOCK_DEVICE — devicePath 指向的不是块设备节点(被 S_ISBLK 拒绝)。
//                      Cabe 设计目标是直接操作裸块设备,普通文件不被接受。
//   QUERY_FAILED     — fstat / ioctl(BLKGETSIZE64) 等查询设备元信息失败。
//   TOO_SMALL        — 设备容量 < 1 个 chunk(CABE_VALUE_DATA_SIZE)。
//   NO_SPACE         — FreeList 自增到设备容量上限,无法再分配新 block;
//                      或 Storage 收到的 blockId 已超出设备容量。
#define DEVICE_NOT_BLOCK_DEVICE (-400005)
#define DEVICE_QUERY_FAILED     (-400006)
#define DEVICE_TOO_SMALL        (-400007)
#define DEVICE_NO_SPACE         (-400008)


// cabe codes
#define CABE_EMPTY_KEY         (-500001)
#define CABE_EMPTY_VALUE       (-500002)
#define CABE_INVALID_DATA_SIZE (-500003)

// crc codes
#define DATA_CRC_MISMATCH (-600001)

// buffer pool codes
#define POOL_ALREADY_INITIALIZED (-700001)
#define POOL_NOT_INITIALIZED     (-700002)
#define POOL_MMAP_FAILED         (-700003)
#define POOL_EXHAUSTED           (-700004)
#define POOL_INVALID_POINTER     (-700005)
#define POOL_INVALID_PARAMS      (-700006)

// free list codes
#define FREE_LIST_DOUBLE_RELEASE (-800001)

// engine 生命周期错误码(与 DEVICE_* 分开,表达的是 Engine 状态机违规,
// 而不是底层设备 I/O 故障):
//   ALREADY_OPEN   — Engine 已打开且本次 Open 的参数(path / buffer_pool_count)
//                    与首次 Open 不一致。同参数再次 Open 仍幂等 SUCCESS。
//   INSTANCE_USED  — Engine 实例此前走过一次 Open→Close,不允许在同一实例上
//                    再 Open(metaIndex/chunkIndex/freeList/nextChunkId 不会被
//                    重置,否则会和新设备内容静默 corruption)。
//                    想重新打开必须销毁此实例构造新实例。
//   NOT_OPEN       — Put/Get/Delete/Remove 在未 Open 或已 Close 状态下被调用。
// 三码都映射到公开 API 的 Status::NotSupported(状态机违规而非 I/O 错误)。
#define ENGINE_ALREADY_OPEN   (-900001)
#define ENGINE_INSTANCE_USED  (-900002)
#define ENGINE_NOT_OPEN       (-900003)

// ============================================================
// IoBackend 抽象层错误码(P3 M1 起新增)
// ============================================================
//
// IoBackend 抽象层独有的错误码,使用 -1000xxx 段位,与底层(DEVICE_*/POOL_*)
// 区分。底层 backend(SyncIoBackend / IoUringIoBackend / SpdkIoBackend)
// 内部仍可使用 DEVICE_* / POOL_* 报告具体故障,在抽象层边界翻译为 IO_BACKEND_*。
//
// 翻译规则(由 engine_api.cpp::TranslateStatus 实现,M3 阶段接入):
//   IO_BACKEND_NOT_OPEN / ALREADY_OPEN / INVALID_HANDLE / HANDLE_USE_AFTER_CLOSE
//                                          → cabe::Status::NotSupported(状态机违规)
//   IO_BACKEND_SUBMIT_FAILED / IO_FAILED   → cabe::Status::IOError
//   IO_BACKEND_POOL_EXHAUSTED              → cabe::Status::ResourceExhausted
//
// 各码的含义:
//   NOT_OPEN                — IoBackend 未打开时调 Read/Write/AcquireBuffer
//   ALREADY_OPEN            — 已打开实例上重复 Open
//   INVALID_HANDLE          — Read/Write 收到 invalid 或跨 backend 的 handle
//   SUBMIT_FAILED           — io_uring submit / SPDK bdev_submit 失败(P4+)
//   IO_FAILED               — CQE res < 0 / bdev I/O 错误
//   POOL_EXHAUSTED          — 所有 buffer slot 都在使用,Acquire 立刻失败(Q3)
//   HANDLE_USE_AFTER_CLOSE  — handle 在 backend Close 之后被访问(Q7)
#define IO_BACKEND_NOT_OPEN                  (-1000001)
#define IO_BACKEND_ALREADY_OPEN              (-1000002)
#define IO_BACKEND_INVALID_HANDLE            (-1000003)
#define IO_BACKEND_SUBMIT_FAILED             (-1000004)
#define IO_BACKEND_IO_FAILED                 (-1000005)
#define IO_BACKEND_POOL_EXHAUSTED            (-1000006)
#define IO_BACKEND_HANDLE_USE_AFTER_CLOSE    (-1000007)
#endif // CABE_ERROR_CODE_H
