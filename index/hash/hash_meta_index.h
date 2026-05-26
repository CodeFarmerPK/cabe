#ifndef CABE_HASH_META_INDEX_H
#define CABE_HASH_META_INDEX_H

#include "index/meta_index.h"
#include "common/error_code.h"
#include "common/structs.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cabe {

    class HashMetaIndex {
    public:
        HashMetaIndex() = default;

        int32_t Insert(std::string_view key, const ValueMeta& meta);
        int32_t Lookup(std::string_view key, ValueMeta* out) const;
        int32_t Delete(std::string_view key);
        std::size_t Size() const noexcept;
        bool Contains(std::string_view key) const;

        void ForEach(MetaIndexVisitor visitor) const;
        int32_t WriteSnapshot(const std::string& path) const;
        int32_t LoadSnapshot(const std::string& path);

    private:
        std::unordered_map<std::string, ValueMeta> map_;
    };

    static_assert(MetaIndexBackend<HashMetaIndex>);

} // namespace cabe

#endif // CABE_HASH_META_INDEX_H
