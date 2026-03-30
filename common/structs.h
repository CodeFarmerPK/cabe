/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:41
 * Created by: CodeFarmerPK
 */
#ifndef STRUCTS_H
#define STRUCTS_H

#include <cstdint>
#include <span>

#define CABE_VALUE_DATA_SIZE (1024 * 1024)

// 数据状态
enum class DataState : uint8_t {
    Active,
    Deleted
};

// Key
using Key = uint64_t;

// BlockId
using BlockId = uint64_t;

// 定义常用 span 类型
using DataView = std::span<const char>;
using DataBuffer = std::span<char>;

// 内存索引
struct IndexEntry {
    BlockId blockId;
    uint64_t timestamp;
    uint32_t crc;
    DataState state;
};
#endif // STRUCTS_H