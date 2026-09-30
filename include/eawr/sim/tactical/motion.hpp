#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// Single-ship space movement (P2-07, #70; docs/behaviour/space-movement.md): move orders,
// acceleration, turning, turning in place and stopping. Everything here is a pure function of
// Q24 values: no clock, thread or host input.
namespace eawr::sim::tactical {

// One unit type's movement: content that the setup's content identity names (for M2 the #65
// unit tables, units::motion_table), neither replay data nor state. Speeds are per logical
// frame and already include Object_Max_Speed_Multiplier_Space (MV-01).
struct MotionProfile {
    TypeId type_id{};
    math::Fixed max_speed{};              // units per frame
    math::Fixed acceleration{};           // units per frame, per frame
    math::Fixed deceleration{};           // units per frame, per frame
    math::Fixed rate_of_turn{};           // degrees per frame
    math::Fixed turn_in_place_slowdown{}; // TurnInPlaceSlowdown* of the type's space layer; 1 otherwise
    // Banking in turns (#351, BK-01): Max_Rate_Of_Roll (degrees per frame, scaled like the rate
    // of turn) and Bank_Turn_Angle (degrees). A zero bank angle never rolls.
    math::Fixed roll_rate{};
    math::Fixed bank_angle{};
    friend constexpr bool operator==(const MotionProfile&, const MotionProfile&) noexcept = default;
};

// gameconstants.xml scalars of the path shape.
struct MotionRules {
    math::Fixed arc_degrees{};        // 360 / MaxRotationsSpace: the turn of one arc step
    math::Fixed expansion_distance{}; // XYExpansionDistanceSpace: the planner's cell scale
    // #452 (docs/behaviour/space-orders.md OR-06, OR-14): MovementReevaluationFrameCount, the
    // frames between approach checks, and Space_Guard_Range. They default to FoC's
    // gameconstants.xml values, 10 frames and 750 units; the unit tables read them at the next
    // FoC identity re-pin (OR-U6).
    std::uint32_t reevaluation_frames{10};
    math::Fixed guard_range{math::Fixed::from_raw(750 * math::Fixed::scale)};
    friend constexpr bool operator==(const MotionRules&, const MotionRules&) noexcept = default;
};

// The tracking system's layers (#71, AV-01, research E71-12): FoC's SpaceLayerType bits.
enum class SpaceLayer : std::uint8_t {
    none = 0,
    capital = 1,
    frigate = 2,
    corvette = 4,
    static_object = 8,
    super_capital = 32,
};

// How the tracking system and the path finder see a type (AV-05, research E71-15, E71-17 to
// E71-19). A ship (a type with a motion profile) is tracked in `layer` with the rectangle of
// its hard half extents; a SPACE_OBSTACLE type is tracked from spawn in `layer` as a square of
// `radius`. `radius` is also the searching ship's soft radius.
struct Footprint {
    TypeId type_id{};
    SpaceLayer layer{SpaceLayer::none};
    math::Fixed x_extent{};
    math::Fixed y_extent{};
    math::Fixed radius{};
    bool obstacle{};
    friend constexpr bool operator==(const Footprint&, const Footprint&) noexcept = default;
};

// gameconstants.xml scalars of the path finder and the tracking system (#71).
struct AvoidanceRules {
    math::Fixed max_rotations{};         // MaxRotationsSpace
    math::Fixed wait_speed{};            // WaitOperatorSpeedCoefficient
    math::Fixed wait_frames{};           // WaitOperatorBaseFrameTime
    math::Fixed wait_cost{};             // WaitOperatorCostCoefficient
    math::Fixed min_obstacle_cost{};     // MinObstacleCostSpace
    math::Fixed path_cost_coefficient{}; // CurrentPathCostCoefficientSpace
    math::Fixed occupation_radius{};     // OccupationRadiusCoefficientSpace
    math::Fixed failure_cutoff{};        // SpacePathFailureDistanceCutoffCoefficient
    math::Fixed failure_expansions{};    // SpacePathFailureMaxExpansionsCoefficient
    math::Fixed failure_rotation{};      // SpacePathFailureRotationExpansionIncrement
    math::Fixed failure_forward{};       // SpacePathFailureForwardExpansionIncrement
    std::uint32_t max_expansions{};      // SpacePathfindMaxExpansions
    std::uint32_t tries{};               // SpacePathingTries
    std::uint32_t tracking_interval{};   // SpaceObjectTrackingInterval: frames per window
    std::uint32_t tracking_windows{};    // SpaceObjectTrackingTreeCount
    math::Fixed destination_search_increment{}; // DestinationSearchRadiusIncrementSpace (#266)
    // Not FoC data: the path-search expansions a dynamic layer may start in one tick
    // (space-movement PC-07, #520). At least 1.
    std::uint64_t search_budget{1000};
    // Not FoC data (PC-08, #520): the frames from a sliced search's start to its landing, and the
    // expansions each of its slices may count before that. At least 1 each.
    std::uint64_t search_delay{4};
    std::uint64_t search_slice{1500};
    friend constexpr bool operator==(const AvoidanceRules&, const AvoidanceRules&) noexcept = default;
};

// An empty table binds no movement: no unit moves or turns, as before #70. A table without
// avoidance rules plans every move without the path finder (MV-12 to MV-17), as before #71.
struct MotionTable {
    MotionRules rules;
    std::vector<MotionProfile> profiles; // strictly increasing type_id
    std::optional<AvoidanceRules> avoidance;
    std::vector<Footprint> footprints; // strictly increasing type_id
    // Craft flight and squadron launch (#75): a craft of a squadron flies by its CraftProfile,
    // never by a MotionProfile.
    SquadronTable squadrons;
    [[nodiscard]] const MotionProfile* find(TypeId type_id) const noexcept;
    [[nodiscard]] const Footprint* footprint(TypeId type_id) const noexcept;
    friend bool operator==(const MotionTable&, const MotionTable&) = default;
};

// Speed, acceleration and turn bounds in whole source units per frame.
inline constexpr std::int64_t max_motion_rate = std::int64_t{1} << 10;
// Positions and targets the planner accepts, in whole source units on each axis.
inline constexpr std::int64_t max_motion_coordinate = std::int64_t{1} << 18;

// Fails with EAWR-SIM-0305 unless type IDs strictly increase, every speed, acceleration,
// deceleration and rate of turn is in (0, max_motion_rate], every slowdown is in
// [1, max_motion_rate], every roll rate is in [0, max_motion_rate] and bank angle in [0, 90],
// the arc angle is in (0, 180] and the expansion distance in (0, max_motion_coordinate], the
// reevaluation interval is 1 to 2^16 frames and the guard range in [0, max_motion_coordinate];
// footprint type IDs strictly increase, extents and radii are in [0, max_motion_coordinate];
// avoidance rules, when present, have 1 to 360 rotations, a wait speed in (0, 1], positive wait
// frames and cost factors, 1 to 2^20 expansions, 1 to 16 tries, a window of 1 to 2^16 frames,
// 1 to 1024 windows and a destination search increment in (0, max_motion_rate].
[[nodiscard]] core::Result<void> validate_motion(const MotionTable& table);

enum class MotionKind : std::uint8_t {
    none = 0, // at rest
    path = 1, // following a planned path (a move order)
    turn = 2, // turning in place (a face order)
};

// One node of a planned path: the frame it is reached (absolute, possibly fractional), where,
// the yaw in degrees in [-180, 180) and the speed there.
struct PathNode {
    math::Fixed frame{};
    math::Vec3 position{};
    math::Fixed yaw{};
    math::Fixed speed{};
    friend constexpr bool operator==(const PathNode&, const PathNode&) noexcept = default;
};

// A unit's movement: kind, the tick it starts, the position, yaw (degrees) and speed at that
// tick, the target and the planned nodes. With the path finder (#71) the nodes also depend on
// the other ships' predictions, so they are part of the state hash.
struct MotionState {
    MotionKind kind{MotionKind::none};
    std::uint64_t start_tick{};
    math::Vec3 start_position{};
    math::Fixed start_yaw{};
    math::Fixed start_speed{};
    math::Vec3 target{};
    std::vector<PathNode> nodes;
    friend bool operator==(const MotionState&, const MotionState&) = default;
};

// Movement limits of a profile after the durability speed factor (engines lost, HD-11): the
// maximum speed, acceleration and deceleration scale by `speed_factor`, the rate of turn does not.
[[nodiscard]] core::Result<MotionProfile> scaled_profile(const MotionProfile& profile, math::Fixed speed_factor);

// Plans a move (MV-10 to MV-19) that starts at `tick` from the given position, yaw and speed.
// Returns kind none (the unit stays) when the target is the start position or outside
// max_motion_coordinate, or the start is outside it.
[[nodiscard]] core::Result<MotionState> plan_move(const MotionProfile& profile, const MotionRules& rules,
    std::uint64_t tick, math::Vec3 position, math::Fixed yaw, math::Fixed speed, math::Vec3 target);

// Plans a turn in place towards `target` (MV-20, MV-21); kind none when there is nothing to turn.
[[nodiscard]] core::Result<MotionState> plan_face(const MotionProfile& profile, std::uint64_t tick,
    math::Vec3 position, math::Fixed yaw, math::Vec3 target);

struct MotionSample {
    math::Vec3 position{};
    math::Fixed yaw{};   // degrees
    math::Fixed speed{}; // units per frame
    bool finished{};     // the plan has ended; the unit is at rest from now on
};

// Where a unit following `state` is at completed tick `tick` (MV-30 to MV-33). `position` and
// `yaw` are the unit's current values, kept where the rules keep them.
[[nodiscard]] core::Result<MotionSample> sample_motion(
    const MotionState& state, std::uint64_t tick, math::Vec3 position, math::Fixed yaw);

// Banking (#351, docs/behaviour/space-movement.md BK-01 to BK-05). The roll is degrees about the
// unit's forward axis; negative lowers its left (+Y) side.
// One frame of a unit that follows a plan and turned from `yaw_before` to `yaw_after` (BK-02):
// the roll eases toward the bank of this frame's turn.
[[nodiscard]] core::Result<math::Fixed> bank_roll(
    const MotionProfile& profile, math::Fixed roll, math::Fixed yaw_before, math::Fixed yaw_after);
// One frame of a unit at rest (BK-04): the roll levels by the full roll rate.
[[nodiscard]] core::Result<math::Fixed> level_roll(const MotionProfile& profile, math::Fixed roll);
// The level `heading` rolled by `roll_degrees` about its forward axis (BK-05); `heading` itself
// when the roll is zero.
[[nodiscard]] core::Result<math::Quat> banked_rotation(math::Quat heading, math::Fixed roll_degrees);

// Yaw in degrees in [-180, 180) of a rotation's forward (+X) axis projected on XY, and the
// level rotation with that yaw (pitch and roll zero).
[[nodiscard]] core::Result<math::Fixed> yaw_degrees(math::Quat rotation);
[[nodiscard]] core::Result<math::Quat> yaw_rotation(math::Fixed yaw_degrees);

[[nodiscard]] std::string_view to_string(MotionKind kind) noexcept;

} // namespace eawr::sim::tactical
