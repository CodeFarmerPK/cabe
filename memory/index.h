/*
 * Project: Cabe
 * Created Time: 2025-05-19 11:23
 * Created by: CodeFarmerPK
 */

#ifndef INDEX_H
#define INDEX_H

#include "common/error_code.h"
#include "common/structs.h"
#include <map>

class Index {
public:
    Index() = default;

    ~Index() = default;

    int32_t Put(Key key, const IndexEntry& entry);

    int32_t Get(Key key, IndexEntry* entry);

    int32_t Delete(Key key);

    int32_t Remove(Key key);


    size_t Size() const;
    bool Contains(Key key) const;

private:
    std::map<Key, IndexEntry> indexMap_;
};

#endif
