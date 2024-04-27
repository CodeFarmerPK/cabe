/*
 * Project: Cabe
 * Created Time: 3/15/24 4:05 PM
 * Created by: CodeFarmerPK
 */

#ifndef CABE_STRUCTS_H
#define CABE_STRUCTS_H

#include <cstdint>

// 内存索引
// 记录在内存中的索引数据结构
struct MemoryIndex {
    uint32_t fileID;
    int64_t offset;
    uint64_t timeStamp;
};

struct MergeFlag {
    bool mergeStart;
    uint64_t mergeStartTime;
};

// 数据状态
// 数据状态标记
enum DataType {
    DataNormal,
    DataDeleted
};

// 数据文件状态
// 数据文件状态标记
enum LogFileType {
    activeFile,
    inactiveFile
};

// 数据文件信息
// 记录数据文件的信息
struct FileInfo {
    uint32_t fileId;
    int64_t offset;
};

// 元数据
// 数据信息的描述
struct Metadata {
    DataType dataType;
    int64_t keySize;
    int64_t valueSize;
    uint64_t timestamp;
    uint32_t crc;

};

// 配置项
struct Options {
    char dataFilePath[32];
    char dataFileSuffix[32];
    char mergeFileSuffix[32];
    int32_t magicCode;
    uint64_t dataFileSize;
    uint64_t preReadSize;
};

#endif //CABE_STRUCTS_H
