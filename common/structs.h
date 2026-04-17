/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:41
 * Created by: CodeFarmerPK
 */
#ifndef STRUCTS_H
#define STRUCTS_H

// Cabe 当前仅支持 Linux（依赖 O_DIRECT / mmap / pread / pwrite / liburing）
// CMake 已在配置阶段做同样检查，此处为源码级兜底，防止跨平台 IDE 误触发构建
#if !defined(__linux__)
#  error "Cabe currently only supports Linux (target: Fedora 43). See README.md."
#endif

#include <cstdint>
#include <span>

#define CABE_VALUE_DATA_SIZE (1024 * 1024)


using ChunkId = uint64_t;   // 逻辑标识，全局自增
using BlockId = uint64_t;   // 物理位置

using DataView = std::span<const char>;
using DataBuffer = std::span<char>;

// 数据状态
enum class DataState : uint8_t {
    Active = 0,
    Deleted = 1
};

struct KeyMeta {
    ChunkId firstChunkId;       // 起始 chunkId
    uint32_t chunkCount;        // chunk 数量
    uint64_t totalSize;         // 实际数据总大小
    uint64_t createdAt;         // 创建时间
    uint64_t modifiedAt;        // 修改时间
    DataState state;            // 状态
};

struct ChunkMeta {
    BlockId blockId;        // 物理块位置
    uint32_t crc;           // CRC 校验
    uint64_t timestamp;     // 写入时间
    DataState state;        // 状态
};

#endif // STRUCTS_H