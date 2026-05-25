#include "engine/meta_index.h"

namespace cabe {

    int32_t MetaIndex::Insert(std::string_view key, const ValueMeta& meta) {
        map_[std::string(key)] = meta;
        return err::kSuccess;
    }

    int32_t MetaIndex::Lookup(std::string_view key, ValueMeta* out) const {
        auto it = map_.find(std::string(key));
        if (it == map_.end()) return err::kIndexKeyNotFound;
        *out = it->second;
        return err::kSuccess;
    }

    int32_t MetaIndex::Delete(std::string_view key) {
        auto it = map_.find(std::string(key));
        if (it == map_.end()) return err::kIndexKeyNotFound;
        map_.erase(it);
        return err::kSuccess;
    }

    std::size_t MetaIndex::Size() const noexcept {
        return map_.size();
    }

    bool MetaIndex::Contains(std::string_view key) const {
        return map_.count(std::string(key)) > 0;
    }

} // namespace cabe
