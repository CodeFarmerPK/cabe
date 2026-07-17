#ifndef CABE_SPDK_TEST_ENV_H
#define CABE_SPDK_TEST_ENV_H

#include "engine/options.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cabe::test {

    using SpdkEnvLookup =
        std::function<std::optional<std::string>(std::string_view name)>;

    enum class SpdkTestConfigState {
        Absent,
        Valid,
        Invalid,
    };

    struct SpdkTestConfigResult {
        SpdkTestConfigState state = SpdkTestConfigState::Absent;
        std::vector<SpdkDeviceConfig> devices;
        std::string error;
    };

    enum class SpdkWritePermission {
        Disabled,
        Enabled,
        Invalid,
    };

    enum class SpdkTestAccess {
        ReadOnly,
        Write,
    };

    enum class SpdkTestAction {
        Run,
        Skip,
        Fail,
    };

    SpdkTestConfigResult ParseSpdkTestConfig(const SpdkEnvLookup& lookup);
    SpdkTestConfigResult ParseSpdkTestConfigFromEnvironment();
    SpdkWritePermission ParseSpdkWritePermission(const SpdkEnvLookup& lookup);
    SpdkWritePermission ParseSpdkWritePermissionFromEnvironment();
    SpdkTestAction DecideSpdkTestAction(SpdkTestConfigState config,
                                        SpdkWritePermission permission,
                                        SpdkTestAccess access) noexcept;

} // namespace cabe::test

#endif // CABE_SPDK_TEST_ENV_H
