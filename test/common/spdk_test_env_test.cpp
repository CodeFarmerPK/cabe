#include "test/common/spdk_test_env.h"

#include "common/error_code.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

using EnvMap = std::unordered_map<std::string, std::string>;

cabe::test::SpdkEnvLookup Lookup(EnvMap env) {
    return [env = std::move(env)](std::string_view name) -> std::optional<std::string> {
        const auto it = env.find(std::string(name));
        if (it == env.end()) return std::nullopt;
        return it->second;
    };
}

void AddGroup(EnvMap& env, std::size_t index, std::string bdf,
              std::uint32_t first_nsid) {
    const std::string prefix = "CABE_TEST_SPDK_G" + std::to_string(index) + "_";
    env[prefix + "DATA_BDF"] = bdf;
    env[prefix + "DATA_NSID"] = std::to_string(first_nsid);
    env[prefix + "WAL_BDF"] = bdf;
    env[prefix + "WAL_NSID"] = std::to_string(first_nsid + 1);
    env[prefix + "SNAPSHOT_BDF"] = std::move(bdf);
    env[prefix + "SNAPSHOT_NSID"] = std::to_string(first_nsid + 2);
}

EnvMap SingleGroupEnv() {
    EnvMap env{{"CABE_TEST_SPDK_GROUP_COUNT", "1"}};
    AddGroup(env, 0, "0000:13:00.0", 1);
    return env;
}

} // namespace

TEST(SpdkTestEnv, MissingCountIsAbsent) {
    const auto result = cabe::test::ParseSpdkTestConfig(Lookup({}));
    EXPECT_EQ(result.state, cabe::test::SpdkTestConfigState::Absent);
    EXPECT_TRUE(result.devices.empty());
    EXPECT_TRUE(result.error.empty());
}

TEST(SpdkTestEnv, CompleteSingleGroupIsValid) {
    const auto result = cabe::test::ParseSpdkTestConfig(Lookup(SingleGroupEnv()));
    ASSERT_EQ(result.state, cabe::test::SpdkTestConfigState::Valid) << result.error;
    ASSERT_EQ(result.devices.size(), 1u);
    EXPECT_EQ(result.devices[0].data.bdf, "0000:13:00.0");
    EXPECT_EQ(result.devices[0].wal.nsid, 2u);
}

TEST(SpdkTestEnv, OptionalRangePairIsParsed) {
    auto env = SingleGroupEnv();
    env["CABE_TEST_SPDK_G0_WAL_OFFSET_BYTES"] = "4096";
    env["CABE_TEST_SPDK_G0_WAL_LENGTH_BYTES"] = "8192";
    const auto result = cabe::test::ParseSpdkTestConfig(Lookup(std::move(env)));
    ASSERT_EQ(result.state, cabe::test::SpdkTestConfigState::Valid) << result.error;
    ASSERT_TRUE(result.devices[0].wal.range.has_value());
    EXPECT_EQ(result.devices[0].wal.range->offset_bytes, 4096u);
    EXPECT_EQ(result.devices[0].wal.range->length_bytes, 8192u);
}

TEST(SpdkTestEnv, CompleteMultipleGroupsAreValid) {
    auto env = SingleGroupEnv();
    env["CABE_TEST_SPDK_GROUP_COUNT"] = "2";
    AddGroup(env, 1, "0000:1b:00.0", 1);
    const auto result = cabe::test::ParseSpdkTestConfig(Lookup(std::move(env)));
    ASSERT_EQ(result.state, cabe::test::SpdkTestConfigState::Valid) << result.error;
    EXPECT_EQ(result.devices.size(), 2u);
}

TEST(SpdkTestEnv, InvalidGroupCountsFail) {
    for (const auto& value : {"0", "-1", "257", "1x", " 1", "18446744073709551616"}) {
        EnvMap env{{"CABE_TEST_SPDK_GROUP_COUNT", value}};
        const auto result = cabe::test::ParseSpdkTestConfig(Lookup(std::move(env)));
        EXPECT_EQ(result.state, cabe::test::SpdkTestConfigState::Invalid) << value;
    }
}

TEST(SpdkTestEnv, MissingRequiredFieldFails) {
    auto env = SingleGroupEnv();
    env.erase("CABE_TEST_SPDK_G0_SNAPSHOT_NSID");
    const auto result = cabe::test::ParseSpdkTestConfig(Lookup(std::move(env)));
    EXPECT_EQ(result.state, cabe::test::SpdkTestConfigState::Invalid);
    EXPECT_TRUE(result.devices.empty());
}

TEST(SpdkTestEnv, GroupIndexGapFails) {
    auto env = SingleGroupEnv();
    env["CABE_TEST_SPDK_GROUP_COUNT"] = "3";
    AddGroup(env, 2, "0000:1b:00.0", 1);
    const auto result = cabe::test::ParseSpdkTestConfig(Lookup(std::move(env)));
    EXPECT_EQ(result.state, cabe::test::SpdkTestConfigState::Invalid);
}

TEST(SpdkTestEnv, NamespaceNumbersRequireStrictDecimal) {
    for (const auto& value : {"", "0x2", "2x", " 2", "2 ", "-2", "+2",
                              "4294967295", "4294967296"}) {
        auto env = SingleGroupEnv();
        env["CABE_TEST_SPDK_G0_DATA_NSID"] = value;
        const auto result = cabe::test::ParseSpdkTestConfig(Lookup(std::move(env)));
        EXPECT_EQ(result.state, cabe::test::SpdkTestConfigState::Invalid) << value;
    }
}

TEST(SpdkTestEnv, RangeRequiresBothFieldsAndStrictDecimal) {
    for (const auto& value : {"1G", "0x1000", " 4096", "-4096", "4096x"}) {
        auto env = SingleGroupEnv();
        env["CABE_TEST_SPDK_G0_DATA_OFFSET_BYTES"] = value;
        env["CABE_TEST_SPDK_G0_DATA_LENGTH_BYTES"] = "4096";
        const auto result = cabe::test::ParseSpdkTestConfig(Lookup(std::move(env)));
        EXPECT_EQ(result.state, cabe::test::SpdkTestConfigState::Invalid) << value;
    }

    auto missing_pair = SingleGroupEnv();
    missing_pair["CABE_TEST_SPDK_G0_DATA_OFFSET_BYTES"] = "0";
    EXPECT_EQ(cabe::test::ParseSpdkTestConfig(Lookup(std::move(missing_pair))).state,
              cabe::test::SpdkTestConfigState::Invalid);
}

TEST(SpdkTestEnv, ProductionValidationRejectsOverlap) {
    auto env = SingleGroupEnv();
    env["CABE_TEST_SPDK_G0_WAL_NSID"] = "1";
    const auto result = cabe::test::ParseSpdkTestConfig(Lookup(std::move(env)));
    EXPECT_EQ(result.state, cabe::test::SpdkTestConfigState::Invalid);
    EXPECT_TRUE(result.devices.empty());
}

TEST(SpdkWritePermission, OnlyExactOneEnablesWrites) {
    EXPECT_EQ(cabe::test::ParseSpdkWritePermission(Lookup({})),
              cabe::test::SpdkWritePermission::Disabled);
    EXPECT_EQ(cabe::test::ParseSpdkWritePermission(
                  Lookup({{"CABE_SPDK_ALLOW_WRITE_TESTS", ""}})),
              cabe::test::SpdkWritePermission::Disabled);
    EXPECT_EQ(cabe::test::ParseSpdkWritePermission(
                  Lookup({{"CABE_SPDK_ALLOW_WRITE_TESTS", "1"}})),
              cabe::test::SpdkWritePermission::Enabled);
    for (const auto& value : {"0", "true", "yes", "on", " 1", "1 "}) {
        EXPECT_EQ(cabe::test::ParseSpdkWritePermission(
                      Lookup({{"CABE_SPDK_ALLOW_WRITE_TESTS", value}})),
                  cabe::test::SpdkWritePermission::Invalid) << value;
    }
}

TEST(SpdkTestDecision, ReadOnlyMatrix) {
    using Action = cabe::test::SpdkTestAction;
    using Config = cabe::test::SpdkTestConfigState;
    using Permission = cabe::test::SpdkWritePermission;
    constexpr auto access = cabe::test::SpdkTestAccess::ReadOnly;

    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Absent, Permission::Disabled, access), Action::Skip);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Absent, Permission::Enabled, access), Action::Skip);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Valid, Permission::Disabled, access), Action::Run);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Valid, Permission::Enabled, access), Action::Run);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Invalid, Permission::Disabled, access), Action::Fail);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Valid, Permission::Invalid, access), Action::Fail);
}

TEST(SpdkTestDecision, WriteMatrix) {
    using Action = cabe::test::SpdkTestAction;
    using Config = cabe::test::SpdkTestConfigState;
    using Permission = cabe::test::SpdkWritePermission;
    constexpr auto access = cabe::test::SpdkTestAccess::Write;

    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Absent, Permission::Disabled, access), Action::Skip);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Absent, Permission::Enabled, access), Action::Fail);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Valid, Permission::Disabled, access), Action::Skip);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Valid, Permission::Enabled, access), Action::Run);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Invalid, Permission::Enabled, access), Action::Fail);
    EXPECT_EQ(cabe::test::DecideSpdkTestAction(Config::Valid, Permission::Invalid, access), Action::Fail);
}
