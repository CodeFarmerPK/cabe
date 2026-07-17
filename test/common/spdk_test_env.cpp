#include "test/common/spdk_test_env.h"

#include "engine/spdk_config.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace cabe::test {
    namespace {
        constexpr std::size_t kMaxSpdkDeviceGroups = 256;

        template <typename UInt>
        std::optional<UInt> ParseUnsignedDecimal(std::string_view value) noexcept {
            static_assert(std::is_unsigned_v<UInt>);
            if (value.empty()) return std::nullopt;
            UInt parsed = 0;
            const char* const begin = value.data();
            const char* const end = begin + value.size();
            const auto result = std::from_chars(begin, end, parsed, 10);
            if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
            return parsed;
        }

        std::optional<std::string> ProcessEnvironment(std::string_view name) {
            const std::string key(name);
            const char* const value = std::getenv(key.c_str());
            if (value == nullptr) return std::nullopt;
            return std::string(value);
        }

        std::string Prefix(std::size_t group, std::string_view role) {
            return "CABE_TEST_SPDK_G" + std::to_string(group) + "_" +
                   std::string(role) + "_";
        }

        bool ParseNamespace(const SpdkEnvLookup& lookup, std::size_t group,
                            std::string_view role, SpdkNvmeNamespaceConfig* output,
                            std::string* error) {
            const std::string prefix = Prefix(group, role);
            const auto bdf = lookup(prefix + "BDF");
            const auto nsid_text = lookup(prefix + "NSID");
            if (!bdf || !nsid_text) {
                *error = "缺少必需环境变量: " + prefix + (!bdf ? "BDF" : "NSID");
                return false;
            }

            const auto nsid = ParseUnsignedDecimal<std::uint32_t>(*nsid_text);
            if (!nsid || *nsid == 0) {
                *error = "非法十进制 nsid: " + prefix + "NSID";
                return false;
            }

            const auto offset_text = lookup(prefix + "OFFSET_BYTES");
            const auto length_text = lookup(prefix + "LENGTH_BYTES");
            if (offset_text.has_value() != length_text.has_value()) {
                *error = "字节范围必须同时提供 offset 与 length: " + prefix;
                return false;
            }

            std::optional<SpdkNvmeByteRange> range;
            if (offset_text) {
                const auto offset = ParseUnsignedDecimal<std::uint64_t>(*offset_text);
                const auto length = ParseUnsignedDecimal<std::uint64_t>(*length_text);
                if (!offset || !length) {
                    *error = "非法十进制字节范围: " + prefix;
                    return false;
                }
                range = SpdkNvmeByteRange{*offset, *length};
            }

            output->bdf = *bdf;
            output->nsid = *nsid;
            output->range = range;
            return true;
        }
    } // namespace

    SpdkTestConfigResult ParseSpdkTestConfig(const SpdkEnvLookup& lookup) {
        SpdkTestConfigResult result;
        const auto count_text = lookup("CABE_TEST_SPDK_GROUP_COUNT");
        if (!count_text) return result;

        const auto count = ParseUnsignedDecimal<std::size_t>(*count_text);
        if (!count || *count == 0 || *count > kMaxSpdkDeviceGroups) {
            result.state = SpdkTestConfigState::Invalid;
            result.error = "CABE_TEST_SPDK_GROUP_COUNT 必须是 1..256 的十进制整数";
            return result;
        }

        std::vector<SpdkDeviceConfig> candidate;
        candidate.resize(*count);
        for (std::size_t group = 0; group < *count; ++group) {
            if (!ParseNamespace(lookup, group, "DATA", &candidate[group].data,
                                &result.error) ||
                !ParseNamespace(lookup, group, "WAL", &candidate[group].wal,
                                &result.error) ||
                !ParseNamespace(lookup, group, "SNAPSHOT", &candidate[group].snapshot,
                                &result.error)) {
                result.state = SpdkTestConfigState::Invalid;
                return result;
            }
        }

        Options options;
        options.spdk_devices = candidate;
        const auto plan = BuildSpdkOpenPlan(options);
        if (!plan.ok()) {
            result.state = SpdkTestConfigState::Invalid;
            result.error = "SPDK 测试设备配置未通过生产配置校验，错误码=" +
                           std::to_string(plan.status.code);
            return result;
        }

        result.state = SpdkTestConfigState::Valid;
        result.devices = std::move(candidate);
        return result;
    }

    SpdkTestConfigResult ParseSpdkTestConfigFromEnvironment() {
        return ParseSpdkTestConfig(ProcessEnvironment);
    }

    SpdkWritePermission ParseSpdkWritePermission(const SpdkEnvLookup& lookup) {
        const auto value = lookup("CABE_SPDK_ALLOW_WRITE_TESTS");
        if (!value || value->empty()) return SpdkWritePermission::Disabled;
        if (*value == "1") return SpdkWritePermission::Enabled;
        return SpdkWritePermission::Invalid;
    }

    SpdkWritePermission ParseSpdkWritePermissionFromEnvironment() {
        return ParseSpdkWritePermission(ProcessEnvironment);
    }

    SpdkTestAction DecideSpdkTestAction(SpdkTestConfigState config,
                                        SpdkWritePermission permission,
                                        SpdkTestAccess access) noexcept {
        if (config == SpdkTestConfigState::Invalid ||
            permission == SpdkWritePermission::Invalid) {
            return SpdkTestAction::Fail;
        }
        if (access == SpdkTestAccess::ReadOnly) {
            return config == SpdkTestConfigState::Valid
                       ? SpdkTestAction::Run
                       : SpdkTestAction::Skip;
        }
        if (config == SpdkTestConfigState::Absent) {
            return permission == SpdkWritePermission::Enabled
                       ? SpdkTestAction::Fail
                       : SpdkTestAction::Skip;
        }
        return permission == SpdkWritePermission::Enabled
                   ? SpdkTestAction::Run
                   : SpdkTestAction::Skip;
    }

} // namespace cabe::test
