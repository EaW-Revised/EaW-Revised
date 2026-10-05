#pragma once

#include "eawr/sim/tactical/type_flags.hpp"
#include <span>
#include <string_view>

namespace eawr::skirmish {
struct DisabledUnit { std::string_view unit; std::string_view reason; std::string_view tooltip; };
struct DisabledAbility { std::string_view unit; std::string_view ability; std::string_view reason; };

// RG-01..05: immutable ship policy generated from the tracked data file.
// Empty reasons mean supported; queries accept XML names in any case.
[[nodiscard]] std::span<const DisabledUnit> roster_disabled_units() noexcept;
[[nodiscard]] const sim::tactical::TypeFlags& roster_disabled_types();
[[nodiscard]] std::string_view roster_disabled_reason(std::string_view unit) noexcept;
[[nodiscard]] std::string_view roster_ability_reason(std::string_view unit, std::string_view ability) noexcept;
} // namespace eawr::skirmish
