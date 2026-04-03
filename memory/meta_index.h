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
