#ifndef CABE_META_INDEX_H
#define CABE_META_INDEX_H

#include "common/structs.h"
#include "common/error_code.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cabe {

    class MetaIndex {
    public:
        MetaIndex() = default;

        int32_t Insert(std::string_view key, const ValueMeta& meta);

        int32_t Lookup(std::string_view key, ValueMeta* out) const;

        int32_t Delete(std::string_view key);

        std::size_t Size() const noexcept;
        bool Contains(std::string_view key) const;

    private:
        std::unordered_map<std::string, ValueMeta> map_;
    };

} // namespace cabe

#endif // CABE_META_INDEX_H
