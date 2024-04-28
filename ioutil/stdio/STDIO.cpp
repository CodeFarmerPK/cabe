/*
 * Project: Cabe
 * Created Time: 3/15/24 4:08 PM
 * Created by: CodeFarmerPK
 */

#include <memory.h>

#include "STDIO.h"

// 初始化文件
int32_t STDIO::Open(const std::string &directory, size_t fileID, const std::string &dataFileSuffix, FILE *&file) {
    std::string fileName = directory + std::to_string(fileID) + dataFileSuffix;
    if (fileName.empty()) {
        return FILE_EMPTY_FILE_NAME;
    }
    file = fopen(fileName.c_str(), "a+");
    if (!file) {
        return FILE_OPEN_FAIL;
    }
    return STATUS_SUCCESS;
};

int32_t STDIO::Write(const std::vector<char> &dataVector, int64_t &dataSize, FILE *&file) {
    if (!file) {
        return FILE_NO_SUCH_FILE;
    }

    if (dataVector.capacity() <= 0 || dataVector.empty()) {
        return FILE_WRITE_FAIL;
    }

    size_t writtenSize = fwrite(dataVector.data(), sizeof(char), dataSize, file);
    if (writtenSize != dataVector.size()) {
        return FILE_WRITE_FAIL;
    }
    return STATUS_SUCCESS;
}

int32_t STDIO::Read(std::vector<char> &valueVector, int64_t offset, size_t &readSize, FILE *&file) {
    if (valueVector.capacity() <= 0) {
        return FILE_READ_FAIL;
    }

    if (!file) {
        return FILE_NO_SUCH_FILE;
    }
    if (fseeko64(file, offset, SEEK_SET) != 0) {
        return FILE_SEEK_FAIL;
    }
    readSize = fread(valueVector.data(), sizeof(char), valueVector.size(), file);
    return STATUS_SUCCESS;
}

int32_t STDIO::Sync(FILE *&file) {
    if (!file) {
        return FILE_NO_SUCH_FILE;
    }
    if (std::fflush(file) == EOF) {
        return FILE_FLUSH_FAIL;
    }
    return STATUS_SUCCESS;
}

int32_t STDIO::Close(FILE *&file) {
    if (!file) {
        return FILE_NO_SUCH_FILE;
    }
    if (fclose(file) != 0) {
        return FILE_CLOSE_FAIL;
    }
    file = nullptr;
    return STATUS_SUCCESS;
}