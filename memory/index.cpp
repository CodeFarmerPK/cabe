/*
 * Project: Cabe
 * Created Time: 2025-05-19 11:23
 * Created by: CodeFarmerPK
 */

#include "index.h"

int32_t Index::Put(const Key key, const IndexEntry& entry) {
    try {
        indexMap_[key] = entry;
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    return SUCCESS;
}

int32_t Index::Get(const Key key, IndexEntry* entry) {
    if (entry == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }

    const auto it = indexMap_.find(key);
    if (it == indexMap_.end()) {
        return INDEX_KEY_NOT_FOUND;
    }

    if (it->second.state != DataState::Active) {
        return INDEX_KEY_DELETED;
    }

    *entry = it->second;
    return SUCCESS;
}

int32_t Index::Delete(const Key key) {
    const auto it = indexMap_.find(key);
    if (it == indexMap_.end()) {
        return INDEX_KEY_NOT_FOUND;
    }

    if (it->second.state == DataState::Deleted) {
        return INDEX_KEY_DELETED;
    }

    it->second.state = DataState::Deleted;
    return SUCCESS;
}

int32_t Index::Remove(const Key key) {
    if (const auto count = indexMap_.erase(key); count == 0) {
        return INDEX_KEY_NOT_FOUND;
    }
    return SUCCESS;
}

size_t Index::Size() const {
    return indexMap_.size();
}

bool Index::Contains(const Key key) const {
    const auto it = indexMap_.find(key);
    if (it == indexMap_.end()) {
        return false;
    }
    return it->second.state == DataState::Active;
}