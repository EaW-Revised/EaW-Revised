#pragma once

#include "eawr/sim/tactical/damage.hpp"
#include <cstddef>
#include <limits>

namespace eawr::sim::tactical::detail {
// Noncanonical construction diagnostics; never selects a trajectory.
struct RocketPathWork {
    std::size_t control_points{}, arc_visits{}, tangent_samples{}, midpoint_estimates{}, required_points{};
};
// Test/session-supplied deterministic storage policy; production uses representability only.
// Never populate this from available memory, wall time or worker count.
struct RocketPathPolicy {
    std::size_t max_controls{std::numeric_limits<std::size_t>::max()};
};
// RFL-01..06: draws angle then radius for interior offsets only. Construction
// commits atomically; a valid but unevaluable route stays initialized.
[[nodiscard]] core::Result<void> prepare_rocket_path(Projectile& projectile,
    CombatRandom* random = nullptr, RocketPathWork* work = nullptr, const RocketPathPolicy& policy = {});
// RFL-07: caller has already selected this frame's point; rebuild only later lookup.
[[nodiscard]] core::Result<void> repath_rocket(Projectile& projectile,
    const math::Vec3& source, const math::Vec3& previous_step,
    RocketPathWork* work = nullptr, const RocketPathPolicy& policy = {});
[[nodiscard]] core::Result<std::optional<math::Vec3>> rocket_point(
    const FlightState& flight, math::Fixed distance);
// RFL-04/09 numerical seam, also used to exercise a single Q24 interval.
[[nodiscard]] core::Result<math::Fixed> rocket_arc_length(const RocketSegment& segment,
    math::Fixed low, math::Fixed high, RocketPathWork& work);
[[nodiscard]] bool valid_flight(const FlightProfile& profile) noexcept;
} // namespace eawr::sim::tactical::detail
