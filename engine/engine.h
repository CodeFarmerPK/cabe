/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:31
 * Created by: CodeFarmerPK
 */

#ifndef CABE_ENGINE_H
#define CABE_ENGINE_H

#include "common/error_code.h"
#include "common/structs.h"
#include "memory/index.h"
#include "storage/free_list.h"
#include "storage/storage.h"
#include <string>
#include <cstdint>

class Engine {
public:
    Engine() = default;
    ~Engine();

    // 初始化引擎
    int32_t Open(const std::string& devicePath);

    // 关闭引擎
    int32_t Close();

    // 写入数据，返回分配的 Key
    int32_t Put(DataView data, Key* key);

    // 读取数据
    int32_t Get(Key key, DataBuffer data);

    // 删除数据（标记删除）
    int32_t Delete(Key key);

    // 真正移除并回收空间
    int32_t Remove(Key key);

    // 获取数据条目数量
    size_t Size() const;

    // 是否已打开
    bool IsOpen() const;

private:
    // 分配对齐内存
    static char* AllocateAlignedBuffer();

    // 释放对齐内存
    static void FreeAlignedBuffer(char* buffer);

private:
    Index index_;
    FreeList freeList_;
    Storage storage_;
    Key nextKey_ = 0;
    bool isOpen_ = false;
};


#endif // CABE_ENGINE_H
