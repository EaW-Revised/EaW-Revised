#pragma once

#include "eawr/sim/tactical/instance.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// #6554, WSPL-06/09: the point a SUPER_LASER beam keeps for use after its target is gone. The simulation holds none, so
// the viewer computes it while the target lives and stamps it with the use it belongs to (the published clip start tick,
// which differs for every use), so that a later use never draws to an earlier use's point.
namespace eawr::presentation::godot_backend::laser_point {

struct Stored final {
    std::array<double, 3> point{};
    std::uint64_t use{};
};

// Twice the owner-to-target offset from the owner (WSPL-06).
[[nodiscard]] inline Stored store(const std::array<double, 3>& owner, const std::array<double, 3>& endpoint,
                                  const std::uint64_t use) noexcept {
    Stored stored;
    stored.use = use;
    for (std::size_t axis = 0; axis < 3; ++axis) stored.point[axis] = owner[axis] + 2.0 * (endpoint[axis] - owner[axis]);
    return stored;
}

// The stored point a continuing beam of this status may draw to: only the one stored by the same use.
[[nodiscard]] inline const std::array<double, 3>* usable(const Stored* stored, const sim::tactical::AbilityStatus& status) noexcept {
    return stored != nullptr && stored->use == status.laser_clip_tick ? &stored->point : nullptr;
}

} // namespace eawr::presentation::godot_backend::laser_point
