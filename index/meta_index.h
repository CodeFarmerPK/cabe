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

    // P5M4：访问器返回 int32_t——回调返"成功"继续、返错误码则 ForEach 立刻停并把错误传出（可中止）。
    using MetaIndexVisitor = std::function<int32_t(std::string_view key, const ValueMeta& meta)>;

    // P5M4：concept 收窄——移除 WriteSnapshot/LoadSnapshot（快照读写 I/O 上移到 snapshot 模块，
    //   后端只保留遍历 + 插入）；ForEach 改为返回 int32_t（可中止，承载"一致扫描"语义）。
    template<typename T>
    concept MetaIndexBackend = requires(T& idx, const T& cidx,
                                        std::string_view key,
                                        const ValueMeta& meta,
                                        ValueMeta* out,
                                        MetaIndexVisitor visitor) {
        { idx.Insert(key, meta) } -> std::same_as<int32_t>;
        { cidx.Lookup(key, out) } -> std::same_as<int32_t>;
        { idx.Delete(key) } -> std::same_as<int32_t>;
        { cidx.Size() } -> std::convertible_to<std::size_t>;
        { cidx.Contains(key) } -> std::same_as<bool>;
        { cidx.ForEach(visitor) } -> std::same_as<int32_t>;
    };

} // namespace cabe

#endif // CABE_META_INDEX_CONCEPT_H
