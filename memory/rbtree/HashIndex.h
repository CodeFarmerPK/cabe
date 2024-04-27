/*
 * Project: Cabe
 * Created Time: 3/19/24 10:56 PM
 * Created by: CodeFarmerPK
 */

#ifndef CABE_HASHINDEX_H
#define CABE_HASHINDEX_H


#include <unordered_map>
#include "../../common/structs.h"
#include "../Index.h"

namespace std {
    template<> struct hash<std::vector<char>> {
        size_t operator()(const std::vector<char>& v) const {
            size_t seed = 0;
            for (char c : v) {
                seed ^= std::hash<char>()(c) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            return seed;
        }
    };
}

class HashIndex : public Index {
public:
    HashIndex() = default;

    ~HashIndex() = default;

    int32_t Put(const std::vector<char> &key, MemoryIndex memoryIndex) override;

    int32_t Get(const std::vector<char> &key, MemoryIndex &memoryIndex) override;

    int32_t Delete(const std::vector<char> &key) override;

    int32_t Persist() override;

    std::unordered_map<std::vector<char>, MemoryIndex> hashIndex;
};

#endif //CABE_HASHINDEX_H
