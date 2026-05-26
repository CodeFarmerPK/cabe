#ifndef CABE_META_INDEX_CONCEPT_H
#define CABE_META_INDEX_CONCEPT_H

#include "common/structs.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cabe {

    using MetaIndexVisitor = std::function<void(std::string_view key, const ValueMeta& meta)>;

    template<typename T>
    concept MetaIndexBackend = requires(T& idx, const T& cidx,
                                        std::string_view key,
                                        const ValueMeta& meta,
                                        ValueMeta* out,
                                        MetaIndexVisitor visitor,
                                        const std::string& path) {
        { idx.Insert(key, meta) } -> std::same_as<int32_t>;
        { cidx.Lookup(key, out) } -> std::same_as<int32_t>;
        { idx.Delete(key) } -> std::same_as<int32_t>;
        { cidx.Size() } -> std::convertible_to<std::size_t>;
        { cidx.Contains(key) } -> std::same_as<bool>;
        { cidx.ForEach(visitor) } -> std::same_as<void>;
        { cidx.WriteSnapshot(path) } -> std::same_as<int32_t>;
        { idx.LoadSnapshot(path) } -> std::same_as<int32_t>;
    };

} // namespace cabe

#endif // CABE_META_INDEX_CONCEPT_H
