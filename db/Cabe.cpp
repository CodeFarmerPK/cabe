/*
 * Project: Cabe
 * Created Time: 3/19/24 11:58 PM
 * Created by: CodeFarmerPK
 */

#include "Cabe.h"

FILE *Cabe::activeFile;
FileInfo *Cabe::activeFileInfo;
Index *Cabe::index;
IOManager *Cabe::ioManager;
Options Cabe::dbOptions{};
uint32_t Cabe::fileId;
int32_t Cabe::metadataSize;
MergeFlag Cabe::mergeFlag;
std::map<uint32_t, FILE *> Cabe::inactiveFiles;
std::map<uint32_t, FILE *> Cabe::mergedFiles;

int32_t Cabe::Put(const std::vector<char> &keyVector, const std::vector<char> &valueVector) {
    if (keyVector.empty()) {
        return CABE_EMPTY_KEY;
    }

    if (valueVector.empty()) {
        return CABE_EMPTY_VALUE;
    }

    int64_t dataSize =
            metadataSize + static_cast<int64_t>( keyVector.size()) + static_cast<int64_t>( valueVector.size());
    std::vector<char> dataVector(dataSize);
    Metadata metadata{DataType::DataNormal,
                      static_cast<int64_t>( keyVector.size()),
                      static_cast<int64_t>( valueVector.size()),
                      getTimeStamp(),
                      calCRC(valueVector, valueVector.size())};
    memcpy(dataVector.data(), &metadata, metadataSize);
    memcpy(dataVector.data() + metadataSize, keyVector.data(), keyVector.size());
    memcpy(dataVector.data() + metadataSize + keyVector.size(), valueVector.data(), valueVector.size());

    // 写入文件
    int32_t status = AppendData(dataVector);
    if (status != STATUS_SUCCESS) {
        // todo 写入失败回滚
        return status;
    }

    // 记录内存
    MemoryIndex memoryIndex{fileId, activeFileInfo->offset - dataSize, getTimeStamp()};
    status = index->Put(keyVector, memoryIndex);
    if (status != STATUS_SUCCESS) {
        // todo 内存插入失败回滚
        return status;
    }
    return STATUS_SUCCESS;
}

int32_t Cabe::Get(const std::vector<char> &keyVector, std::vector<char> &valueVector) {
    if (keyVector.empty()) {
        return MEMORY_EMPTY_KEY;
    }
    int32_t status;
    MemoryIndex memoryIndex{};
    status = index->Get(keyVector, memoryIndex);
    if (status != STATUS_SUCCESS) {
        return status;
    }

    FILE *dataFile;
    if (memoryIndex.fileID == fileId) {
        dataFile = activeFile;
    } else {
        if (mergeFlag.mergeStart) {
            if (memoryIndex.timeStamp < mergeFlag.mergeStartTime) {
                //读旧文件
                dataFile = inactiveFiles[memoryIndex.fileID];
                if (!dataFile) {
                    status = ioManager->Open(dbOptions.dataFilePath, fileId, dbOptions.dataFileSuffix, dataFile);
                    if (status != STATUS_SUCCESS) {
                        return status;
                    }
                    inactiveFiles[activeFileInfo->fileId] = dataFile;
                }
            }

            if (memoryIndex.timeStamp > mergeFlag.mergeStartTime) {
                //读新文件
                dataFile = mergedFiles[memoryIndex.fileID];
            }
        } else {
            dataFile = inactiveFiles[memoryIndex.fileID];
            if (!dataFile) {
                status = ioManager->Open(dbOptions.dataFilePath, fileId, dbOptions.dataFileSuffix, dataFile);
                if (status != STATUS_SUCCESS) {
                    return status;
                }
                inactiveFiles[activeFileInfo->fileId] = dataFile;
            }
        }
    }

    size_t readSize = 0;
    std::vector<char> preVector(dbOptions.preReadSize);
    // 从文件中读取预读大小的字节
    status = ioManager->Read(preVector, memoryIndex.offset, readSize, dataFile);
    if (status != STATUS_SUCCESS) {
        return status;
    }
    auto *metadata = reinterpret_cast<Metadata *>(preVector.data());
    if (metadata->dataType == DataDeleted) {
        return CABE_DATA_NOT_FOUND;
    }

    int64_t keySize = metadata->keySize;
    int64_t valueSize = metadata->valueSize;
    // 超过预读大小时，再读一次文件
    valueVector.resize(valueSize);
    if (metadataSize + keySize + valueSize > dbOptions.preReadSize) {
        status = ioManager->Read(valueVector, memoryIndex.offset + metadataSize + keySize, readSize, dataFile);
        if (status != STATUS_SUCCESS) {
            return status;
        }

    } else {
        memcpy(valueVector.data(), preVector.data() + metadataSize + keySize, valueSize);
    }

    if (metadata->crc != calCRC(valueVector, valueSize)) {
        return CABE_ERROR_DATA;
    }

    return STATUS_SUCCESS;
}

int32_t Cabe::Delete(const std::vector<char> &keyVector) {
    if (keyVector.empty()) {
        return CABE_EMPTY_KEY;
    }
    MemoryIndex memoryIndex{};
    int32_t status = index->Get(keyVector, memoryIndex);
    if (status != STATUS_SUCCESS) {
        return status;
    }

    Metadata metadata{DataType::DataDeleted,
                      static_cast<int64_t>(keyVector.size()),
                      0,
                      getTimeStamp(),
                      0};
    metadata.keySize = static_cast<int64_t>(keyVector.size());
    std::vector<char> valueVector(metadataSize + keyVector.size());
    memcpy(valueVector.data(), &metadata, metadataSize);
    memcpy(valueVector.data() + metadataSize, keyVector.data(), keyVector.size());

    status = AppendData(valueVector);
    if (status != STATUS_SUCCESS) {
        return status;
    }
    status = index->Delete(keyVector);
    if (status != STATUS_SUCCESS) {
        return status;
    }
    return STATUS_SUCCESS;
}

int32_t Cabe::Merge() {
    // todo 暂时只考虑正常流程，不考虑异常处理

    // 仅merge非活跃文件
    auto mergeFiles(inactiveFiles);
    if (mergeFiles.empty()) {
        return CABE_MERGE_ERROR;
    }

    mergeFlag.mergeStart = true;
    mergeFlag.mergeStartTime = getTimeStamp();

    std::vector<char> preVector(dbOptions.preReadSize);

    // todo 索引插入和crc计算通过指针地址进行，无需分配vector
    std::vector<char> keyVector;
    std::vector<char> valueVector;

    FILE *mergeFile;
    Metadata metadata;            // 文件索引信息
    MemoryIndex mergeIndex;       // 内存索引信息
    size_t readSize = 0;          // 每次读取的大小
    int64_t readOffset = 0;       // 旧文件的偏移量，每次读旧文件之后更新，用于控制文件是否读完
    int64_t totalSize = 0;        // 本次读取的数据总大小

    uint32_t mergeFileId = 0;     // 正在merge的文件id
    int64_t mergeOffset = 0;      // 正在merge的文件偏移量，每次写merge文件之后更新，用于控制文件是否需要开启新的merge文件

    ioManager->Open(dbOptions.dataFilePath, mergeFileId, dbOptions.mergeFileSuffix, mergeFile);
    mergeFiles[mergeFileId] = mergeFile;
    for (auto &item: mergeFiles) {
        while (true) {
            // 一个旧文件读完就删除它，从内存中删除打开文件的索引，并退出本次循环
            if (readOffset >= dbOptions.dataFileSize) {
                ioManager->Close(item.second);
                inactiveFiles.erase(item.first);
                std::filesystem::remove(dbOptions.dataFilePath + std::to_string(item.first) + dbOptions.dataFileSuffix);
                break;
            }
            // 读旧文件数据，readOffset为文件偏移量，readSize为读取的文件大小
            ioManager->Read(preVector, readOffset, readSize, item.second);
            metadata = *reinterpret_cast<Metadata *>(preVector.data());
            totalSize = metadataSize + metadata.keySize + metadata.valueSize;

            if (metadata.dataType == DataType::DataNormal) {
                if (totalSize > dbOptions.preReadSize) {
                    // 数据大小超出预读大小，重新读取一次
                    preVector.resize(totalSize);
                    ioManager->Read(preVector, readOffset, readSize, item.second);
                }

                keyVector.resize(metadata.keySize);
                memcpy(keyVector.data(), preVector.data() + metadataSize, keyVector.size());
                index->Get(keyVector, mergeIndex);
                if (mergeIndex.fileID == item.first && mergeIndex.offset == readOffset) {
                    valueVector.resize(metadata.valueSize);
                    memcpy(valueVector.data(), preVector.data() + metadataSize + metadata.keySize, valueVector.size());
                    if (metadata.crc != calCRC(valueVector, valueVector.size())) {
                        readOffset += totalSize;
                        continue;
                    }

                    // todo AppendData函数需要支持merge，统一处理
                    if (mergeOffset >= dbOptions.dataFileSize) {
                        ioManager->Sync(mergeFile);
                        mergeFileId++;
                        ioManager->Open(dbOptions.dataFilePath, mergeFileId, dbOptions.mergeFileSuffix, mergeFile);
                        mergeFiles[mergeFileId] = mergeFile;
                    }
                    ioManager->Write(preVector, totalSize, activeFile);

                    mergeIndex.fileID = mergeFileId;
                    mergeIndex.offset = mergeOffset;
                    mergeIndex.timeStamp = getTimeStamp();
                    index->Put(keyVector, mergeIndex);
                    // 成功写入时更新merge文件偏移量
                    mergeOffset += totalSize;
                }
            }
            // 更新已读旧文件偏移量
            readOffset += totalSize;
        }
    }


    for (auto &item: mergeFiles) {
        ioManager->Sync(item.second);
        ioManager->Close(item.second);
        std::string oldFilePath = dbOptions.dataFilePath + std::to_string(item.first) + dbOptions.mergeFileSuffix;
        std::string newFilePath = dbOptions.dataFilePath + std::to_string(item.first) + dbOptions.dataFileSuffix;
        std::filesystem::rename(oldFilePath, newFilePath);
    }
    mergeFiles.clear();
    mergeFlag.mergeStart = false;
    mergeFlag.mergeStartTime = getTimeStamp();
    return STATUS_SUCCESS;
}

int32_t Cabe::Persist() {
    // 将文件ID持久化
    return STATUS_SUCCESS;
}

int32_t Cabe::AppendData(const std::vector<char> &dataVector) {
    // 不存在活跃文件时,初始化活跃文件
    if (!activeFileInfo) {
        fileId = 0;
        ioManager->Open(dbOptions.dataFilePath, fileId, dbOptions.dataFileSuffix, activeFile);
        activeFileInfo = new FileInfo{fileId, 0};
    }

    // 活跃文件写满时,转为非活跃文件并创建新的活跃文件
    if (activeFileInfo->offset >= dbOptions.dataFileSize) {
        inactiveFiles[activeFileInfo->fileId] = activeFile;
        fileId++;
        ioManager->Sync(activeFile);

        ioManager->Open(dbOptions.dataFilePath, fileId, dbOptions.dataFileSuffix, activeFile);
        activeFileInfo = new FileInfo{fileId, 0};
    }

    // 追加写入文件并更新当前文件偏移量
    int64_t dataSize = dataVector.size();
    ioManager->Write(dataVector, dataSize, activeFile);
    activeFileInfo->offset += static_cast<int64_t>(dataVector.size());
    return STATUS_SUCCESS;
}

int32_t Cabe::LoadDataFile() {
    // TODO 加载内存索引
    // TODO 加载文件记录
    return STATUS_SUCCESS;
}