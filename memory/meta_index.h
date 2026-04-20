/*
 * Project: Cabe
 * Created Time: 2025-05-19 11:23
 * Created by: CodeFarmerPK
 */

#ifndef CABE_META_INDEX_H
#define CABE_META_INDEX_H

#include "common/error_code.h"
#include "common/structs.h"
#include <string>
#include <unordered_map>

class MetaIndex {
public:
    MetaIndex() = default;
    ~MetaIndex() = default;

    // 索引层是 Engine 内部的可变状态，复制 / 移动语义无业务含义
    // （两个 MetaIndex 持同样 key 的两份独立 KeyMeta 会立刻分裂）。
    // 与项目内其他持状态的类保持一致的 = delete 纪律。
    MetaIndex(const MetaIndex&) = delete;
    MetaIndex& operator=(const MetaIndex&) = delete;
    MetaIndex(MetaIndex&&) = delete;
    MetaIndex& operator=(MetaIndex&&) = delete;

    int32_t Put(const std::string& key, const KeyMeta& meta);

    int32_t Get(const std::string& key, KeyMeta* meta) const;

    int32_t Delete(const std::string& key);

    int32_t Remove(const std::string& key);

    size_t Size() const;

    bool Contains(const std::string& key) const;

private:
    std::unordered_map<std::string, KeyMeta> indexMap_;
};

#endif // CABE_META_INDEX_H
