/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:41
 * Created by: CodeFarmerPK
 */
#ifndef STRUCTS_H
#define STRUCTS_H

#include <cstdint>

#define CABE_VALUE_DATA_SIZE (1024 * 1024)

// 数据状态
enum class DataState : uint8_t {
    Active,
    Deleted
};

// Key
using Key = uint64_t;

// 内存索引
struct IndexEntry {
    uint64_t blockId;
    uint64_t timestamp;
    uint32_t crc;
    DataState state;
};
#endif // STRUCTS_H