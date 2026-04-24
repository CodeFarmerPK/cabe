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
#endif // CABE_ERROR_CODE_H
