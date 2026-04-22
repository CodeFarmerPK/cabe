/*
 * Project: Cabe
 * Created Time: 2025-05-19 11:23
 * Created by: CodeFarmerPK
 */

#include "meta_index.h"

int32_t MetaIndex::Put(const std::string& key, const KeyMeta& meta) {
    try {
        auto [it, inserted] = indexMap_.try_emplace(key, meta);
        const bool newActive = (meta.state == DataState::Active);
        if (inserted) {
            // 新插入：仅当新 entry 是 Active 才计入活跃数。
            // P2 ::Engine::Put 总传 Active，此处 if 必然为真；
            // 但 P4 WAL 重放可能直接 Put(state=Deleted) 重建已删条目，那时
            // 这条 if 守住了"Deleted entry 不算 active"的不变式。
            if (newActive) ++activeCount_;
        } else {
            // 覆盖写：根据 state 跃迁四种组合调整 activeCount_
            //   Active → Active   : 不变
            //   Active → Deleted  : --（被删）
            //   Deleted → Active  : ++（复活）
            //   Deleted → Deleted : 不变
            const bool oldActive = (it->second.state == DataState::Active);
            if (!oldActive && newActive) {
                ++activeCount_;
            } else if (oldActive && !newActive) {
                --activeCount_;
            }
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
