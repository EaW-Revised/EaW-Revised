#pragma once

#include "eawr/sim/tactical/damage.hpp"
#include <cstddef>

namespace eawr::sim::tactical::detail {
// Noncanonical construction diagnostics; bounds apply independently of worker count.
struct RocketPathWork {
    std::size_t control_points{}, arc_visits{}, tangent_samples{};
};
inline constexpr std::size_t rocket_point_limit = 2048;
inline constexpr std::size_t rocket_arc_visit_limit = 131072;
// RFL-01..06: draws angle then radius for interior offsets only. Construction
// commits atomically; a valid but unevaluable route stays initialized.
[[nodiscard]] core::Result<void> prepare_rocket_path(Projectile& projectile,
    CombatRandom* random = nullptr, RocketPathWork* work = nullptr);
// RFL-07: caller has already selected this frame's point; rebuild only later lookup.
[[nodiscard]] core::Result<void> repath_rocket(Projectile& projectile,
    const math::Vec3& source, const math::Vec3& previous_step);
[[nodiscard]] core::Result<std::optional<math::Vec3>> rocket_point(
    const FlightState& flight, math::Fixed distance);
[[nodiscard]] bool valid_flight(const FlightProfile& profile) noexcept;
} // namespace eawr::sim::tactical::detail
