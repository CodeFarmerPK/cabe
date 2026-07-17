#include "engine/engine.h"
#include "engine/spdk_config.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

cabe::SpdkNvmeNamespaceConfig Ns(std::string bdf, std::uint32_t nsid) {
    return {.bdf = std::move(bdf), .nsid = nsid, .range = std::nullopt};
}

cabe::SpdkNvmeNamespaceConfig Ns(std::string bdf, std::uint32_t nsid,
                                 std::uint64_t offset, std::uint64_t length) {
    return {.bdf = std::move(bdf),
            .nsid = nsid,
            .range = cabe::SpdkNvmeByteRange{offset, length}};
}

cabe::SpdkDeviceConfig Group(std::string bdf = "0000:13:00.0",
                             std::uint32_t first_nsid = 1) {
    return {.data = Ns(bdf, first_nsid),
            .wal = Ns(bdf, first_nsid + 1),
            .snapshot = Ns(std::move(bdf), first_nsid + 2)};
}

cabe::Options SpdkOptions(cabe::SpdkDeviceConfig group = Group()) {
    cabe::Options options;
    options.spdk_devices.push_back(std::move(group));
    return options;
}

} // namespace

TEST(SpdkConfigFamily, StrictMatrix) {
    cabe::Options none;
    EXPECT_EQ(cabe::ValidateDeviceConfigFamily(none, cabe::DeviceConfigFamily::Raw),
              cabe::err::kSpdkInvalidConfig);
    EXPECT_EQ(cabe::ValidateDeviceConfigFamily(none, cabe::DeviceConfigFamily::Spdk),
              cabe::err::kSpdkInvalidConfig);

    cabe::Options raw;
    raw.devices.push_back({"data", "wal", "snapshot"});
    EXPECT_EQ(cabe::ValidateDeviceConfigFamily(raw, cabe::DeviceConfigFamily::Raw),
              cabe::err::kSuccess);
    EXPECT_EQ(cabe::ValidateDeviceConfigFamily(raw, cabe::DeviceConfigFamily::Spdk),
              cabe::err::kSpdkInvalidConfig);

    auto spdk = SpdkOptions();
    EXPECT_EQ(cabe::ValidateDeviceConfigFamily(spdk, cabe::DeviceConfigFamily::Spdk),
              cabe::err::kSuccess);
    EXPECT_EQ(cabe::ValidateDeviceConfigFamily(spdk, cabe::DeviceConfigFamily::Raw),
              cabe::err::kSpdkInvalidConfig);

    spdk.devices.push_back({"data", "wal", "snapshot"});
    EXPECT_EQ(cabe::ValidateDeviceConfigFamily(spdk, cabe::DeviceConfigFamily::Raw),
              cabe::err::kSpdkInvalidConfig);
    EXPECT_EQ(cabe::ValidateDeviceConfigFamily(spdk, cabe::DeviceConfigFamily::Spdk),
              cabe::err::kSpdkInvalidConfig);
}

TEST(SpdkConfig, BuildsCanonicalImmutablePlan) {
    auto options = SpdkOptions(Group("ABCD:EF:1A.7", 10));
    const auto result = cabe::BuildSpdkOpenPlan(options);
    ASSERT_TRUE(result.ok()) << result.status.code;
    ASSERT_EQ(result.plan.size(), 1u);

    const auto& data = result.plan.devices()[0].data;
    EXPECT_EQ(data.bdf.domain, 0xabcdu);
    EXPECT_EQ(data.bdf.bus, 0xefu);
    EXPECT_EQ(data.bdf.device, 0x1au);
    EXPECT_EQ(data.bdf.function, 7u);
    EXPECT_EQ(data.bdf.text, "abcd:ef:1a.7");
    EXPECT_EQ(data.nsid, 10u);
    EXPECT_FALSE(data.range.has_value());

    EXPECT_EQ(options.spdk_devices[0].data.bdf, "ABCD:EF:1A.7");
}

TEST(SpdkConfig, RejectsInvalidBdfForms) {
    const std::vector<std::string> invalid{
        "13:00.0", "0000:13:00", "0000:13:00.00", " 0000:13:00.0",
        "0000:13:00.0 ", "0000:gg:00.0", "0000:13:20.0", "0000:13:00.8",
        "00000:13:00.0", "0000-13:00.0",
    };
    for (const auto& bdf : invalid) {
        auto options = SpdkOptions(Group(bdf, 1));
        const auto result = cabe::BuildSpdkOpenPlan(options);
        EXPECT_EQ(result.status.code, cabe::err::kSpdkInvalidConfig) << bdf;
        EXPECT_TRUE(result.plan.empty()) << bdf;
    }
}

TEST(SpdkConfig, RejectsReservedNamespaceIds) {
    for (const auto nsid : {0u, std::numeric_limits<std::uint32_t>::max()}) {
        auto group = Group();
        group.wal.nsid = nsid;
        const auto result = cabe::BuildSpdkOpenPlan(SpdkOptions(std::move(group)));
        EXPECT_EQ(result.status.code, cabe::err::kSpdkInvalidConfig) << nsid;
        EXPECT_TRUE(result.plan.empty()) << nsid;
    }
}

TEST(SpdkConfig, FailureDoesNotReturnPartialPlan) {
    cabe::Options options;
    options.spdk_devices.push_back(Group("0000:13:00.0", 1));
    options.spdk_devices.push_back(Group("invalid", 4));
    const auto result = cabe::BuildSpdkOpenPlan(options);
    EXPECT_EQ(result.status.code, cabe::err::kSpdkInvalidConfig);
    EXPECT_TRUE(result.plan.empty());
    EXPECT_EQ(options.spdk_devices.size(), 2u);
}

TEST(SpdkConfig, AcceptsAdjacentRangesOnSameNamespace) {
    cabe::SpdkDeviceConfig group{
        .data = Ns("0000:13:00.0", 1, 0, 4096),
        .wal = Ns("0000:13:00.0", 1, 4096, 4096),
        .snapshot = Ns("0000:13:00.0", 1, 8192, 4096),
    };
    const auto result = cabe::BuildSpdkOpenPlan(SpdkOptions(std::move(group)));
    ASSERT_TRUE(result.ok()) << result.status.code;
}

TEST(SpdkConfig, RejectsInvalidRanges) {
    const std::vector<cabe::SpdkNvmeByteRange> invalid{
        {0, 0},
        {1, 4096},
        {0, 4097},
        {std::numeric_limits<std::uint64_t>::max() - 4095, 8192},
    };
    for (const auto range : invalid) {
        auto group = Group();
        group.data.range = range;
        const auto result = cabe::BuildSpdkOpenPlan(SpdkOptions(std::move(group)));
        EXPECT_EQ(result.status.code, cabe::err::kSpdkInvalidConfig)
            << "offset=" << range.offset_bytes << " length=" << range.length_bytes;
    }
}

TEST(SpdkConfig, RejectsWholeNamespaceWithAnotherView) {
    auto group = Group();
    group.data = Ns("0000:13:00.0", 1);
    group.wal = Ns("0000:13:00.0", 1, 0, 4096);
    const auto result = cabe::BuildSpdkOpenPlan(SpdkOptions(std::move(group)));
    EXPECT_EQ(result.status.code, cabe::err::kSpdkNamespaceOverlap);
    EXPECT_TRUE(result.plan.empty());
}

TEST(SpdkConfig, RejectsPartialAndContainingOverlap) {
    for (const auto& wal_range : std::vector<cabe::SpdkNvmeByteRange>{
             {4096, 8192},
             {0, 16384},
             {0, 8192},
         }) {
        cabe::SpdkDeviceConfig group{
            .data = Ns("0000:13:00.0", 1, 0, 12288),
            .wal = Ns("0000:13:00.0", 1, wal_range.offset_bytes,
                      wal_range.length_bytes),
            .snapshot = Ns("0000:13:00.0", 2),
        };
        const auto result = cabe::BuildSpdkOpenPlan(SpdkOptions(std::move(group)));
        EXPECT_EQ(result.status.code, cabe::err::kSpdkNamespaceOverlap);
    }
}

TEST(SpdkConfig, ChecksOverlapAcrossGroupsAndRoles) {
    cabe::Options options;
    options.spdk_devices.push_back({
        .data = Ns("0000:13:00.0", 1, 0, 8192),
        .wal = Ns("0000:13:00.0", 2),
        .snapshot = Ns("0000:13:00.0", 3),
    });
    options.spdk_devices.push_back({
        .data = Ns("0000:1b:00.0", 1),
        .wal = Ns("0000:13:00.0", 4),
        .snapshot = Ns("0000:13:00.0", 1, 4096, 4096),
    });
    const auto result = cabe::BuildSpdkOpenPlan(options);
    EXPECT_EQ(result.status.code, cabe::err::kSpdkNamespaceOverlap);
}

TEST(SpdkConfig, DifferentControllerOrNamespaceDoesNotConflict) {
    auto options = SpdkOptions(Group("0000:13:00.0", 1));
    options.spdk_devices.push_back(Group("0000:1b:00.0", 1));
    EXPECT_TRUE(cabe::BuildSpdkOpenPlan(options).ok());

    options.spdk_devices[1] = Group("0000:13:00.0", 10);
    EXPECT_TRUE(cabe::BuildSpdkOpenPlan(options).ok());
}

TEST(SpdkConfig, EnforcesDeviceGroupLimit) {
    cabe::Options options;
    for (std::uint32_t i = 0; i < 256; ++i) {
        options.spdk_devices.push_back(Group("0000:13:00.0", i * 3 + 1));
    }
    EXPECT_TRUE(cabe::BuildSpdkOpenPlan(options).ok());

    options.spdk_devices.push_back(Group("0000:1b:00.0", 1));
    const auto result = cabe::BuildSpdkOpenPlan(options);
    EXPECT_EQ(result.status.code, cabe::err::kSpdkInvalidConfig);
    EXPECT_TRUE(result.plan.empty());
}

TEST(SpdkEnginePlaceholder, ValidConfigFailsBeforeOpeningResources) {
    cabe::Engine engine;
    const auto status = engine.Open(SpdkOptions());
    EXPECT_EQ(status.code, cabe::err::kEngineNotImplemented);
    EXPECT_FALSE(engine.is_open());
    EXPECT_EQ(engine.Close().code, cabe::err::kEngineNotOpen);
}

TEST(SpdkEnginePlaceholder, RawOrMixedConfigIsRejected) {
    cabe::Engine engine;
    cabe::Options raw;
    raw.devices.push_back({"data", "wal", "snapshot"});
    EXPECT_EQ(engine.Open(raw).code, cabe::err::kSpdkInvalidConfig);

    auto mixed = SpdkOptions();
    mixed.devices.push_back({"data", "wal", "snapshot"});
    EXPECT_EQ(engine.Open(mixed).code, cabe::err::kSpdkInvalidConfig);
    EXPECT_FALSE(engine.is_open());
}
