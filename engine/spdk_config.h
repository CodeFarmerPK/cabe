#ifndef CABE_SPDK_CONFIG_H
#define CABE_SPDK_CONFIG_H

#include "engine/options.h"
#include "engine/status.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cabe {

    enum class DeviceConfigFamily : std::uint8_t {
        Raw,
        Spdk,
    };

    enum class SpdkDeviceRole : std::uint8_t {
        Data,
        Wal,
        Snapshot,
    };

    struct CanonicalBdf {
        std::uint16_t domain = 0;
        std::uint8_t bus = 0;
        std::uint8_t device = 0;
        std::uint8_t function = 0;
        std::string text;

        auto operator<=>(const CanonicalBdf&) const = default;
    };

    struct ValidatedSpdkNamespacePlan {
        CanonicalBdf bdf;
        std::uint32_t nsid = 0;
        std::optional<SpdkNvmeByteRange> range;
    };

    struct ValidatedSpdkDevicePlan {
        ValidatedSpdkNamespacePlan data;
        ValidatedSpdkNamespacePlan wal;
        ValidatedSpdkNamespacePlan snapshot;
    };

    struct SpdkOpenPlanResult;

    // Open 阶段只消费这份规范化后的不可变计划，不再重新解释调用方 Options。
    class SpdkOpenPlan {
    public:
        const std::vector<ValidatedSpdkDevicePlan>& devices() const noexcept {
            return devices_;
        }
        std::size_t size() const noexcept { return devices_.size(); }
        bool empty() const noexcept { return devices_.empty(); }

    private:
        std::vector<ValidatedSpdkDevicePlan> devices_;

        friend SpdkOpenPlanResult BuildSpdkOpenPlan(const Options& options);
    };

    struct SpdkOpenPlanResult {
        Status status;
        SpdkOpenPlan plan;

        bool ok() const noexcept { return status.ok(); }
    };

    int32_t ValidateDeviceConfigFamily(const Options& options,
                                       DeviceConfigFamily expected) noexcept;
    SpdkOpenPlanResult BuildSpdkOpenPlan(const Options& options);

} // namespace cabe

#endif // CABE_SPDK_CONFIG_H
