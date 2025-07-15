/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:41
 * Created by: CodeFarmerPK
 */
#ifndef STRUCTS_H
#define STRUCTS_H

#include <cstdint>

#define DATA_SIZE 1048576

// 内存索引
// 记录在内存中的索引数据结构
struct MemoryIndex {
    int64_t offset;
    uint64_t timeStamp;
};

// 数据状态
// 数据状态标记
enum DataType { DataNormal, DataDeleted };

// 元数据
// 数据信息的描述
struct Metadata {
    DataType dataType;

    int64_t keySize;
    int64_t valueSize;
    uint64_t timestamp;
    uint32_t crc;
};
#endif // STRUCTS_H
