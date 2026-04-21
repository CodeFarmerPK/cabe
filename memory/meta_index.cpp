/*
 * Project: Cabe
 * Created Time: 2025-05-19 11:23
 * Created by: CodeFarmerPK
 */

#include "meta_index.h"

int32_t MetaIndex::Put(const std::string& key, const KeyMeta& meta) {
    try {
        auto [it, inserted] = indexMap_.try_emplace(key, meta);
        if (inserted) {
            ++activeCount_;
        } else {
            // 覆盖写：若旧条目是 Deleted，激活计数要补回来
            if (it->second.state == DataState::Deleted) ++activeCount_;
            it->second = meta;
        }
    } catch (...) {
        return MEMORY_INSERT_FAIL;
    }
    return SUCCESS;
}

int32_t MetaIndex::Get(const std::string& key, KeyMeta* meta) const {
    if (meta == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }

    const auto it = indexMap_.find(key);
    if (it == indexMap_.end()) {
        return INDEX_KEY_NOT_FOUND;
    }

    *meta = it->second;

    if (it->second.state != DataState::Active) {
        return INDEX_KEY_DELETED;
    }

    return SUCCESS;
}

int32_t MetaIndex::Delete(const std::string& key) {
    const auto it = indexMap_.find(key);
    if (it == indexMap_.end()) {
        return INDEX_KEY_NOT_FOUND;
    }

    if (it->second.state == DataState::Deleted) {
        return INDEX_KEY_DELETED;
    }

    it->second.state = DataState::Deleted;
    --activeCount_;
    return SUCCESS;
}

int32_t MetaIndex::Remove(const std::string& key) {
    const auto it = indexMap_.find(key);
    if (it == indexMap_.end()) {
        return INDEX_KEY_NOT_FOUND;
    }
    if (it->second.state == DataState::Active) --activeCount_;
    indexMap_.erase(it);
    return SUCCESS;
}

size_t MetaIndex::Size() const {
    return activeCount_;
}

bool MetaIndex::Contains(const std::string& key) const {
    const auto it = indexMap_.find(key);
    if (it == indexMap_.end()) {
        return false;
    }
    return it->second.state == DataState::Active;
}
