#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Group moves (#344, docs/behaviour/space-movement.md FM-01 to FM-12): FoC's
// space movement coordinator turns one move order for several ships into a destination per
// ship (its slot), a planning speed per ship and a staggered planning frame per ship. Pure
// functions of Q24 values: no clock, thread or host input.
namespace eawr::sim::tactical {

// gameconstants.xml SpacePathfindFrameDelayDelta (FM-08): frames between two ships of one layer
// planning their paths.
inline constexpr std::uint32_t pathfind_frame_delay = 2;

// A ship of a group move as the mapping sees it at the order's frame.
struct FormationMember {
    EntityId entity{};
    math::Vec3 position{};
    math::Fixed yaw{};              // degrees
    SpaceLayer layer{SpaceLayer::none};
    math::Fixed occupation_radius{}; // FM-04
    math::Fixed max_speed{};         // after the durability factor (MV-33)
    math::Fixed rate_of_turn{};      // degrees per frame
    math::Fixed soft_radius{};       // the footprint's soft radius (AV-19's query square)
    bool asteroid_damage{};          // WHZ-08a: behavior opt-in selects field avoidance
    bool through_hazards{};
};

// One ship's part of a group move: where it goes, the maximum speed it plans with and the
// frames after the order's frame at which it plans (0: at once).
struct FormationSlot {
    EntityId entity{};
    math::Vec3 destination{};
    math::Fixed max_speed{};
    std::uint32_t delay{};
    friend constexpr bool operator==(const FormationSlot&, const FormationSlot&) noexcept = default;
};

// FM-04 (research E344-07): the hard radius (the diagonal of the hard half extents) times
// OccupationRadiusCoefficientSpace plus the turn radius at `max_speed` times
// WaitOperatorSpeedCoefficient.
[[nodiscard]] core::Result<math::Fixed> occupation_radius(
    const Footprint& footprint, math::Fixed max_speed, math::Fixed rate_of_turn, const AvoidanceRules& rules);

// FM-06 (research E344-03): the time a ship needs to face and reach `target`.
[[nodiscard]] core::Result<math::Fixed> time_to_reach(const FormationMember& member, math::Vec3 target);

// FM-02 to FM-09: the slots of a group move ordered at `frame` towards `target`. `members`
// are the ships in command order, each in a dynamic layer; `world` holds the tracking layers
// and the static layer as they stand before any member plans. The result lists every member
// once, in the order the ships plan: layer by layer (capital, frigate, corvette, super
// capital), the front ship of each layer first. `rules` supplies the nearest open position's
// constants (AV-19).
[[nodiscard]] core::Result<std::vector<FormationSlot>> map_group_move(std::span<const FormationMember> members,
    math::Vec3 target, const CollisionWorld& world, std::uint64_t frame, const AvoidanceRules& rules);

} // namespace eawr::sim::tactical
