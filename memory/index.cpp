/*
 * Project: Cabe
 * Created Time: 2025-05-19 11:23
 * Created by: CodeFarmerPK
 */

#include "index.h"
std::unordered_map<std::string_view, MemoryIndex> Index::memoryIndexMap;

int32_t Index::Put(const std::string_view& key, MemoryIndex& memoryIndex) {
    auto res = memoryIndexMap.insert_or_assign (key, std::move(memoryIndex));
    if (res.second) {
        return SUCCESS;
    }
    return MEMORY_INSERT_FAIL;
}

int32_t Index::Get(const std::string_view& key, MemoryIndex& memoryIndex) {
    auto it = memoryIndexMap.find(key);
    if (it == memoryIndexMap.end()) {
        return MEMORY_EMPTY_KEY;
    }
    memoryIndex = it->second;
    return SUCCESS;
}

int32_t Index::Delete(const std::string_view& key) {
    auto it = memoryIndexMap.find(key);
    if (it == memoryIndexMap.end()) {
        return MEMORY_EMPTY_KEY;
    }
    memoryIndexMap.erase(it);
    return SUCCESS;
}

int32_t Index::Persist() {
    return SUCCESS;
}
