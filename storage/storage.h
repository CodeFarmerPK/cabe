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
    Storage() = default;
    ~Storage();

    // Storage 持有独占资源 fd_，默认拷贝/移动会让两个对象共享同一 fd，
    // 析构时 double-close → UB。显式 delete 让错误调用在编译期就暴露。
    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;
    Storage(Storage&&) = delete;
    Storage& operator=(Storage&&) = delete;
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
