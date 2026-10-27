#pragma once

#include <cstdint>

namespace eawr::presentation {

// Leave the established device-memory slack for allocations in flight and drivers.
inline constexpr std::uint64_t device_budget_usable_numerator = 9;
inline constexpr std::uint64_t device_budget_usable_denominator = 10;

[[nodiscard]] constexpr std::uint64_t usable_device_budget_bytes(const std::uint64_t bytes) noexcept {
    return bytes / device_budget_usable_denominator * device_budget_usable_numerator;
}

} // namespace eawr::presentation
