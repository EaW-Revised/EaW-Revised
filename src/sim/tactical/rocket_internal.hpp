#pragma once

#include "eawr/sim/tactical/damage.hpp"

namespace eawr::sim::tactical::detail {
// RFL-02..04: zero-offset, target-terminating space route; other constructions
// remain the weapons follow-up. A valid but unevaluable route stays initialized.
[[nodiscard]] core::Result<void> prepare_rocket_path(Projectile& projectile);
[[nodiscard]] core::Result<std::optional<math::Vec3>> rocket_point(
    const FlightState& flight, math::Fixed distance);
[[nodiscard]] bool valid_flight(const FlightProfile& profile) noexcept;
} // namespace eawr::sim::tactical::detail
