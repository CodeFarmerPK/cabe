#ifndef CABE_STATUS_H
#define CABE_STATUS_H

#include "common/error_code.h"

#include <compare>
#include <type_traits>

namespace cabe {

    struct Status {
        int code = err::kSuccess;

        constexpr bool ok() const noexcept { return code == err::kSuccess; }
        constexpr explicit operator bool() const noexcept { return ok(); }

        static constexpr Status Ok() noexcept { return Status{err::kSuccess}; }
        static constexpr Status Error(int c) noexcept { return Status{c}; }

        constexpr auto operator<=>(const Status&) const noexcept = default;
    };

    static_assert(sizeof(Status) == sizeof(int));
    static_assert(std::is_trivially_copyable_v<Status>);

} // namespace cabe

#endif // CABE_STATUS_H
