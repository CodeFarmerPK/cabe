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
    // 打开裸块设备。
    //
    // 严格契约(Cabe 是直接操作裸设备的存储引擎,不接受普通文件):
    //   - devicePath 必须是已存在的块设备节点(/dev/nvmeXn1, /dev/sdX, /dev/loopX 等)
    //   - 通过 fstat + S_ISBLK 校验,普通文件/字符设备返回 DEVICE_NOT_BLOCK_DEVICE
    //   - 通过 ioctl(BLKGETSIZE64) 取设备字节数;失败返回 DEVICE_QUERY_FAILED
    //   - 设备容量 < 1 个 chunk(CABE_VALUE_DATA_SIZE)返回 DEVICE_TOO_SMALL
    //   - 不创建、不 truncate、不 unlink——这三个操作对块设备节点都危险或无意义
    int32_t Open(const std::string& devicePath);

    // 关闭块设备
    int32_t Close();

    // 写入指定 block（定长）。
    // 越界(blockId >= blockCount_)返回 DEVICE_NO_SPACE;
    // 正常路径上 FreeList 已先于此处拦住,这里是最后一道防线。
    int32_t WriteBlock(BlockId blockId, DataView data) const;

    // 读取指定 block（定长）。越界保护同 WriteBlock。
    int32_t ReadBlock(BlockId blockId, DataBuffer data) const;

    // 是否已打开
    bool IsOpen() const;
    // 设备能容纳的 block 数量(向下取整 = ioctl(BLKGETSIZE64) / CABE_VALUE_DATA_SIZE)。
    //
    // 设计选择:对外暴露 block 数而非字节数。Cabe 是定长 value 引擎,字节
    // 容量在引擎内部没有任何意义,所有寻址都按 blockId 进行。让 Storage 层
    // 一次性完成"字节 → block 数"翻译,上层(Engine / FreeList)直接用 block
    // 数,避免到处出现 `/ CABE_VALUE_DATA_SIZE` 的算式重复。
    //
    // Open 成功后 >= 1(否则 Open 阶段就返回 DEVICE_TOO_SMALL),Close 后归 0。
    uint64_t BlockCount() const;
private:
    int fd_ = -1;
    std::string devicePath_;
    // 设备能容纳的 block 数量,Open 时一次性算定,运行期不变。
    // 字节 → block 数的向下取整丢弃的尾部不足 1 chunk 的字节,
    // 在裸设备上是不可寻址区(blockId 不会指到那里),无副作用。
    uint64_t blockCount_ = 0;
};


#endif // CABE_STORAGE_H
