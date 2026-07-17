#include "engine/spdk_config.h"

#include "common/error_code.h"
#include "common/logger.h"
#include "common/structs.h"

#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace cabe {
    namespace {
        constexpr std::size_t kMaxSpdkDeviceGroups = 256;
        constexpr std::uint32_t kNvmeGlobalNamespaceId =
            std::numeric_limits<std::uint32_t>::max();
        constexpr std::array<SpdkDeviceRole, 3> kRoles{
            SpdkDeviceRole::Data,
            SpdkDeviceRole::Wal,
            SpdkDeviceRole::Snapshot,
        };

        const char* RoleName(SpdkDeviceRole role) noexcept {
            switch (role) {
                case SpdkDeviceRole::Data: return "data";
                case SpdkDeviceRole::Wal: return "wal";
                case SpdkDeviceRole::Snapshot: return "snapshot";
            }
            return "unknown";
        }

        const SpdkNvmeNamespaceConfig& NamespaceConfig(const SpdkDeviceConfig& config,
                                                        SpdkDeviceRole role) noexcept {
            switch (role) {
                case SpdkDeviceRole::Data: return config.data;
                case SpdkDeviceRole::Wal: return config.wal;
                case SpdkDeviceRole::Snapshot: return config.snapshot;
            }
            return config.data;
        }

        ValidatedSpdkNamespacePlan& NamespacePlan(ValidatedSpdkDevicePlan& plan,
                                                  SpdkDeviceRole role) noexcept {
            switch (role) {
                case SpdkDeviceRole::Data: return plan.data;
                case SpdkDeviceRole::Wal: return plan.wal;
                case SpdkDeviceRole::Snapshot: return plan.snapshot;
            }
            return plan.data;
        }

        std::optional<unsigned> ParseHex(std::string_view text) noexcept {
            unsigned value = 0;
            for (const unsigned char ch : text) {
                unsigned digit = 0;
                if (ch >= '0' && ch <= '9') {
                    digit = ch - '0';
                } else if (ch >= 'a' && ch <= 'f') {
                    digit = ch - 'a' + 10;
                } else if (ch >= 'A' && ch <= 'F') {
                    digit = ch - 'A' + 10;
                } else {
                    return std::nullopt;
                }
                value = value * 16 + digit;
            }
            return value;
        }

        std::optional<CanonicalBdf> ParseBdf(std::string_view text) {
            if (text.size() != 12 || text[4] != ':' || text[7] != ':' || text[10] != '.') {
                return std::nullopt;
            }

            const auto domain = ParseHex(text.substr(0, 4));
            const auto bus = ParseHex(text.substr(5, 2));
            const auto device = ParseHex(text.substr(8, 2));
            const auto function = ParseHex(text.substr(11, 1));
            if (!domain || !bus || !device || !function || *device > 0x1f || *function > 7) {
                return std::nullopt;
            }

            std::string canonical(text);
            for (char& ch : canonical) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return CanonicalBdf{
                .domain = static_cast<std::uint16_t>(*domain),
                .bus = static_cast<std::uint8_t>(*bus),
                .device = static_cast<std::uint8_t>(*device),
                .function = static_cast<std::uint8_t>(*function),
                .text = std::move(canonical),
            };
        }

        bool ValidRange(const SpdkNvmeByteRange& range) noexcept {
            if (range.length_bytes == 0) return false;
            if (range.offset_bytes % kSuperBlockSize != 0 ||
                range.length_bytes % kSuperBlockSize != 0) {
                return false;
            }
            return range.offset_bytes <=
                   std::numeric_limits<std::uint64_t>::max() - range.length_bytes;
        }

        bool SameNamespace(const ValidatedSpdkNamespacePlan& left,
                           const ValidatedSpdkNamespacePlan& right) noexcept {
            return left.bdf.domain == right.bdf.domain &&
                   left.bdf.bus == right.bdf.bus &&
                   left.bdf.device == right.bdf.device &&
                   left.bdf.function == right.bdf.function &&
                   left.nsid == right.nsid;
        }

        bool RangesOverlap(const ValidatedSpdkNamespacePlan& left,
                           const ValidatedSpdkNamespacePlan& right) noexcept {
            if (!left.range || !right.range) return true;
            const std::uint64_t left_end = left.range->offset_bytes + left.range->length_bytes;
            const std::uint64_t right_end = right.range->offset_bytes + right.range->length_bytes;
            return left.range->offset_bytes < right_end && right.range->offset_bytes < left_end;
        }

        struct PlanEntryRef {
            std::size_t group = 0;
            SpdkDeviceRole role = SpdkDeviceRole::Data;
            const ValidatedSpdkNamespacePlan* plan = nullptr;
        };
    } // namespace

    int32_t ValidateDeviceConfigFamily(const Options& options,
                                       DeviceConfigFamily expected) noexcept {
        const bool has_raw = !options.devices.empty();
        const bool has_spdk = !options.spdk_devices.empty();
        const bool exactly_one = has_raw != has_spdk;
        const bool expected_present = expected == DeviceConfigFamily::Raw ? has_raw : has_spdk;
        if (!exactly_one || !expected_present) {
            CABE_LOG_ERROR("设备配置族非法: raw=%d spdk=%d expected=%s",
                           has_raw, has_spdk,
                           expected == DeviceConfigFamily::Raw ? "raw" : "spdk");
            return err::kSpdkInvalidConfig;
        }
        return err::kSuccess;
    }

    SpdkOpenPlanResult BuildSpdkOpenPlan(const Options& options) {
        SpdkOpenPlanResult result{Status::Error(err::kSpdkInvalidConfig), {}};
        if (ValidateDeviceConfigFamily(options, DeviceConfigFamily::Spdk) != err::kSuccess) {
            return result;
        }
        if (options.spdk_devices.size() > kMaxSpdkDeviceGroups) {
            CABE_LOG_ERROR("SPDK 设备组过多: %zu > %zu",
                           options.spdk_devices.size(), kMaxSpdkDeviceGroups);
            return result;
        }

        std::vector<ValidatedSpdkDevicePlan> candidate(options.spdk_devices.size());

        // 固定阶段一：先规范化全部 BDF，避免失败结果携带部分可执行计划。
        for (std::size_t group = 0; group < options.spdk_devices.size(); ++group) {
            for (const auto role : kRoles) {
                const auto& source = NamespaceConfig(options.spdk_devices[group], role);
                auto bdf = ParseBdf(source.bdf);
                if (!bdf) {
                    CABE_LOG_ERROR("SPDK BDF 非法: group=%zu role=%s bdf='%s'",
                                   group, RoleName(role), source.bdf.c_str());
                    return result;
                }
                NamespacePlan(candidate[group], role).bdf = std::move(*bdf);
            }
        }

        // 固定阶段二：再校验全部 nsid。
        for (std::size_t group = 0; group < options.spdk_devices.size(); ++group) {
            for (const auto role : kRoles) {
                const auto& source = NamespaceConfig(options.spdk_devices[group], role);
                if (source.nsid == 0 || source.nsid == kNvmeGlobalNamespaceId) {
                    CABE_LOG_ERROR("SPDK nsid 非法: group=%zu role=%s bdf=%s nsid=%u",
                                   group, RoleName(role), source.bdf.c_str(), source.nsid);
                    return result;
                }
                NamespacePlan(candidate[group], role).nsid = source.nsid;
            }
        }

        // 固定阶段三：最后校验并复制字节范围。
        for (std::size_t group = 0; group < options.spdk_devices.size(); ++group) {
            for (const auto role : kRoles) {
                const auto& source = NamespaceConfig(options.spdk_devices[group], role);
                if (source.range && !ValidRange(*source.range)) {
                    CABE_LOG_ERROR(
                        "SPDK 字节范围非法: group=%zu role=%s bdf=%s nsid=%u offset=%llu length=%llu",
                        group, RoleName(role), source.bdf.c_str(), source.nsid,
                        static_cast<unsigned long long>(source.range->offset_bytes),
                        static_cast<unsigned long long>(source.range->length_bytes));
                    return result;
                }
                NamespacePlan(candidate[group], role).range = source.range;
            }
        }

        std::vector<PlanEntryRef> entries;
        entries.reserve(candidate.size() * kRoles.size());
        for (std::size_t group = 0; group < candidate.size(); ++group) {
            for (const auto role : kRoles) {
                entries.push_back({group, role, &NamespacePlan(candidate[group], role)});
            }
        }

        // 全局检查所有组和所有角色；同一 namespace 的整盘视图与任何其他视图冲突。
        for (std::size_t left = 0; left < entries.size(); ++left) {
            for (std::size_t right = left + 1; right < entries.size(); ++right) {
                if (!SameNamespace(*entries[left].plan, *entries[right].plan) ||
                    !RangesOverlap(*entries[left].plan, *entries[right].plan)) {
                    continue;
                }
                CABE_LOG_ERROR(
                    "SPDK namespace 范围重叠: bdf=%s nsid=%u; "
                    "left(group=%zu role=%s kind=%s offset=%llu length=%llu); "
                    "right(group=%zu role=%s kind=%s offset=%llu length=%llu)",
                    entries[left].plan->bdf.text.c_str(), entries[left].plan->nsid,
                    entries[left].group, RoleName(entries[left].role),
                    entries[left].plan->range ? "range" : "whole",
                    static_cast<unsigned long long>(
                        entries[left].plan->range ? entries[left].plan->range->offset_bytes : 0),
                    static_cast<unsigned long long>(
                        entries[left].plan->range ? entries[left].plan->range->length_bytes : 0),
                    entries[right].group, RoleName(entries[right].role),
                    entries[right].plan->range ? "range" : "whole",
                    static_cast<unsigned long long>(
                        entries[right].plan->range ? entries[right].plan->range->offset_bytes : 0),
                    static_cast<unsigned long long>(
                        entries[right].plan->range ? entries[right].plan->range->length_bytes : 0));
                result.status = Status::Error(err::kSpdkNamespaceOverlap);
                return result;
            }
        }

        result.plan.devices_ = std::move(candidate);
        result.status = Status::Ok();
        return result;
    }

} // namespace cabe
