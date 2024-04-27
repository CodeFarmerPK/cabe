/*
 * Project: Cabe
 * Created Time: 3/19/24 10:56 PM
 * Created by: CodeFarmerPK
 */

#include "HashIndex.h"

int32_t HashIndex::Put(const std::vector<char> &key, MemoryIndex memoryIndex) {
    hashIndex[key] = memoryIndex;
    return STATUS_SUCCESS;
}

int32_t HashIndex::Get(const std::vector<char> &key, MemoryIndex &memoryIndex) {
    auto item = hashIndex.find(key);
    if (item == hashIndex.end()) {
        return MEMORY_EMPTY_KEY;
    }
    memoryIndex = item->second;
    return STATUS_SUCCESS;
}

int32_t HashIndex::Delete(const std::vector<char> &key) {
    auto item = hashIndex.find(key);
    if (item == hashIndex.end()) {
        return MEMORY_EMPTY_KEY;
    }
    hashIndex.erase(item);
    return STATUS_SUCCESS;
}

int32_t HashIndex::Persist() {
    // 将内存索引持久化
    return STATUS_SUCCESS;
}