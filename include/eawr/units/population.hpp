#pragma once

#include <optional>
#include <type_traits>

namespace eawr::units {

// WR-03, WGT-07: a nonnegative space override replaces the base value,
// including zero. An absent or negative override leaves the base in effect.
template <typename T>
[[nodiscard]] constexpr T population_in_space(const std::optional<T>& base,
    const std::optional<T>& space_override) noexcept {
    if constexpr (std::is_unsigned_v<T>) {
        // The unsigned tactical loader already maps negative sentinels to absence.
        return space_override.value_or(base.value_or(T{}));
    } else {
        return space_override && *space_override >= T{} ? *space_override : base.value_or(T{});
    }
}

} // namespace eawr::units
