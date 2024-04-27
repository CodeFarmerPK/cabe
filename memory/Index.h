/*
 * Project: Cabe
 * Created Time: 3/19/24 10:53 PM
 * Created by: CodeFarmerPK
 */

#ifndef CABE_INDEX_H
#define CABE_INDEX_H

#include <vector>
#include "../common/structs.h"
#include "../common/error_code.h"

class Index {
public:
    virtual int32_t Put(const std::vector<char> &key, MemoryIndex memoryIndex) = 0;

    // Get 根据 key 取出对应的索引位置信息
    virtual int32_t Get(const std::vector<char> &key, MemoryIndex &memoryIndex) = 0;

    // Delete 根据 key 删除对应的索引位置信息
    virtual int32_t Delete(const std::vector<char> &key) = 0;

    virtual int32_t Persist() = 0;
};

#endif //CABE_INDEX_H
