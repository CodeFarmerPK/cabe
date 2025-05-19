/*
 * Project: Cabe
 * Created Time: 2025-05-19 11:23
 * Created by: CodeFarmerPK
 */
 
#ifndef INDEX_H
#define INDEX_H

#include <unordered_map>

#include "common/error_code.h"
#include "common/structs.h"

class Index {
public:
    Index() = default;

    ~Index() = default;

    int32_t Put(const std::span<char> &key, MemoryIndex memoryIndex);

    int32_t Get(const std::span<char> &key, MemoryIndex &memoryIndex);

    int32_t Delete(const std::span<char> &key);

    int32_t Persist();

private:
    std::unordered_map<std::span<char>, MemoryIndex> hashIndex;
};

#endif
