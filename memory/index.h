/*
 * Project: Cabe
 * Created Time: 2025-05-19 11:23
 * Created by: CodeFarmerPK
 */

#ifndef INDEX_H
#define INDEX_H

#include "common/error_code.h"
#include "common/structs.h"
#include <string_view>
#include <unordered_map>

class Index {
public:
    Index() = default;

    ~Index() = default;

    int32_t Put(const std::string_view& key, MemoryIndex& memoryIndex);

    int32_t Get(const std::string_view& key, MemoryIndex& memoryIndex);

    int32_t Delete(const std::string_view& key);

    int32_t Persist();

private:
    static std::unordered_map<std::string_view, MemoryIndex> memoryIndexMap;
};

#endif
