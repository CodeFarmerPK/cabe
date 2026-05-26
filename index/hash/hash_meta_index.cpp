#include "index/hash/hash_meta_index.h"

namespace cabe {

    int32_t HashMetaIndex::Insert(std::string_view key, const ValueMeta& meta) {
        map_[std::string(key)] = meta;
        return err::kSuccess;
    }

    int32_t HashMetaIndex::Lookup(std::string_view key, ValueMeta* out) const {
        auto it = map_.find(std::string(key));
        if (it == map_.end()) return err::kIndexKeyNotFound;
        *out = it->second;
        return err::kSuccess;
    }

    int32_t HashMetaIndex::Delete(std::string_view key) {
        auto it = map_.find(std::string(key));
        if (it == map_.end()) return err::kIndexKeyNotFound;
        map_.erase(it);
        return err::kSuccess;
    }

    std::size_t HashMetaIndex::Size() const noexcept {
        return map_.size();
    }

    bool HashMetaIndex::Contains(std::string_view key) const {
        return map_.count(std::string(key)) > 0;
    }

    void HashMetaIndex::ForEach(MetaIndexVisitor visitor) const {
        for (const auto& [key, meta] : map_) {
            visitor(key, meta);
        }
    }

    int32_t HashMetaIndex::WriteSnapshot(const std::string& path) const {
        (void)path;
        return err::kEngineNotImplemented;
    }

    int32_t HashMetaIndex::LoadSnapshot(const std::string& path) {
        (void)path;
        return err::kEngineNotImplemented;
    }

} // namespace cabe
