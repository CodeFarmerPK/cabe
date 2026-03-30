/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:21
 * Created by: CodeFarmerPK
 */

#ifndef CABE_STORAGE_H
#define CABE_STORAGE_H

#include "common/error_code.h"
#include "common/structs.h"
#include <string>
#include <cstdint>

class Storage {
public:
public:
    Storage() = default;
    ~Storage();

    // 打开块设备
    int32_t Open(const std::string& devicePath);

    // 关闭块设备
    int32_t Close();

    // 写入指定 block（定长）
    int32_t WriteBlock(BlockId blockId, DataView data) const;

    // 读取指定 block（定长）
    int32_t ReadBlock(BlockId blockId, DataBuffer data) const;

    // 是否已打开
    bool IsOpen() const;

private:
    int fd_ = -1;
    std::string devicePath_;
};


#endif // CABE_STORAGE_H
