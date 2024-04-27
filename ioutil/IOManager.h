/*
 * Project: Cabe
 * Created Time: 3/15/24 4:07 PM
 * Created by: CodeFarmerPK
 */

#ifndef CABE_IOMANAGER_H
#define CABE_IOMANAGER_H

#include <string>
#include <vector>


class IOManager {
public:

    // 初始化文件
    virtual int32_t Open(const std::string &directory, uint64_t fileID, const std::string &fileSuffix, FILE *&file) = 0;

    // 将字节数组写入文件
    virtual int32_t Write(const std::vector<char> &dataVector, FILE *&file) = 0;

    // 从文件中给定位置读取数据
    virtual int32_t Read(std::vector<char> &valueVector, int64_t offset, size_t &readSize, FILE *&File) = 0;

    // 持久化数据
    virtual int32_t Sync(FILE *&file) = 0;

    // 关闭文件
    virtual int32_t Close(FILE *&file) = 0;
};

#endif //CABE_IOMANAGER_H
