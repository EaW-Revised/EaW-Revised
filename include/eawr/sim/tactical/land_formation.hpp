#pragma once

#include "eawr/sim/tactical/land_placement.hpp"
#include "eawr/units/land_companies.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Land formations (docs/behaviour/walks/land-pathing.md, LMF-01..LMF-08): a group move order puts nearby objects into
// formations, gives every formation one destination and every object a slot in it, and moves a formation along one
// centre path that each object walks at its own offset. Pure functions of the terrain, the committed occupancy and
// their inputs; the session owns the order, the search and the retained records.
namespace eawr::sim::tactical {

// LMF-02..LMF-05: the game constants a formation order reads (`Max_Formation_Area`, the two recruitment distances,
// `BetweenFormationSpacing`, `Destination_Collision_Query_Extension`), already in world units. A table that does not
// author all of them has `authored` false and group orders keep the one-destination-per-squad rule.
struct LandFormationSettings {
    bool authored{};
    math::Fixed max_area{}, vehicle_recruit{}, infantry_recruit{}, between_spacing{}, query_extension{};
    bool speed_authored{};
    math::Fixed minimum_side_error{}, maximum_side_error{};
    friend bool operator==(const LandFormationSettings&, const LandFormationSettings&) = default;
};

// One addressed squad (a team container or a single object) as a formation object. The half extents are the squad's
// own oriented box (forward, side); `spacing` is its formation spacing already times 20 (WLPA-24); `recruit` is the
// recruitment distance that applies to it (LMF-02).
struct LandFormationObject {
    EntityId id{};
    math::Vec2 position{};
    math::Fixed half_forward{}, half_side{}, radius{}, spacing{}, range{}, speed{}, recruit{};
    std::uint32_t mask{};
    units::LandFormationGrouping grouping{units::LandFormationGrouping::standard};
    bool hero{}, infantry_class{}, giant{}, formation_team{};
};

// A body outside the order that a formation box may not overlap (LMF-05): its box at the end slot of its walk.
struct LandFormationObstacle {
    math::Vec2 position{}, axis{math::Fixed::from_raw(math::Fixed::scale), {}};
    math::Fixed half_forward{}, half_side{};
    bool infantry_class{}, formation_team{};
};

// An object's slot: forward (positive toward the heading) and side (positive to the left of the heading) from the
// formation destination.
struct LandFormationSlot {
    EntityId id{};
    math::Vec2 offset{};
    friend bool operator==(const LandFormationSlot&, const LandFormationSlot&) = default;
};

struct LandFormationPlan {
    EntityId lead{};                       // the seed object; its search carries the centre path
    std::vector<LandFormationSlot> slots;  // rank order (LMF-06)
    math::Vec2 start{}, destination{}, heading{};
    std::uint32_t mask{};                  // the common movement class (WLPA-12)
    math::Fixed radius{}, width{};         // the centre path request: largest soft radius, twice the side extent (LMF-07)
    bool forced{};                         // every placement try failed and the box stands on the click (LMF-04)
};

// Deterministic work of one plan (scratch, never state): the obstacles that were in reach of the click and the box tests
// made against them.
struct LandFormationWork {
    std::uint64_t obstacles_kept{}, obstacle_tests{};
};

// LMF-09: each observation is the neighbour's forward slot difference and signed path distance.
struct LandFormationObservation {
    math::Fixed slot_difference{}, forward_distance{};
};
// Empty or entirely dead-band observations yield zero. nullopt indicates arithmetic overflow.
[[nodiscard]] std::optional<math::Fixed> land_formation_deviance(std::span<const LandFormationObservation> observations);
// Formation speed with zero lateral correction. Inputs are per-second speeds and deceleration; nullopt indicates
// invalid negative speed/deceleration or arithmetic overflow. The extrema include zero.
[[nodiscard]] std::optional<math::Fixed> land_formation_speed(math::Fixed deviance, math::Fixed maximum_deviance,
    math::Fixed minimum_deviance, math::Fixed formation_maximum, math::Fixed object_maximum,
    math::Fixed deceleration, bool enforce_top_speed = false);
// LMF-09: signed forward distance between positions on centre-path segments, crossing corner bisector half-planes.
[[nodiscard]] std::optional<math::Fixed> land_formation_forward_offset(std::span<const math::Vec2> centre,
    std::uint32_t first_segment, math::Vec2 first, std::uint32_t second_segment, math::Vec2 second);
// Advance only through consecutive crossed half-planes, never beyond the installed member-path correspondence.
[[nodiscard]] std::optional<std::uint32_t> land_formation_advance_segment(std::span<const math::Vec2> centre,
    std::uint32_t segment, std::uint32_t limit, math::Vec2 position);

// LMF-01..LMF-06: groups `objects` (any order; sorted by id here) into formations and places them around `click`.
// nullopt on arithmetic overflow or no objects; the caller keeps the per-squad rule then.
[[nodiscard]] std::optional<std::vector<LandFormationPlan>> land_plan_formations(const LandOccupancy& occupancy,
    const LandFormationSettings& settings, std::span<const LandFormationObject> objects,
    std::span<const LandFormationObstacle> obstacles, math::Vec2 click, LandFormationWork* work = nullptr);

// LMF-07: the path one object walks along `centre` (the formation's centre path, first node at its start) at its slot,
// starting where it stands. The centre path is sampled every cell; each sample is offset in the local heading frame,
// and an offset sample that the object's class cannot stand on narrows toward the centre line (a quarter at a time), so a
// column squeezes through a gap. nullopt when no legal route results; the caller then searches for that object alone. The last node stands at the slot in the frame of
// `final_heading` (the plan's heading, the frame placement tested), not of the last path segment. When a sample would
// make the walk double back (a turn of more than a right angle at the last node, or a step behind its own heading), the
// nodes it doubles back on are cut, so a member on the inside of a corner does not run against its facing.
[[nodiscard]] std::optional<std::vector<math::Vec2>> land_formation_member_path(const LandOccupancy& occupancy,
    std::span<const math::Vec2> centre, math::Vec2 member_start, math::Vec2 slot, std::uint32_t mask, math::Fixed radius,
    math::Vec2 final_heading, std::vector<std::uint32_t>* centre_segments = nullptr);

// The slot's world position for a heading (the forward unit vector).
[[nodiscard]] std::optional<math::Vec2> land_formation_slot_position(math::Vec2 destination, math::Vec2 heading, math::Vec2 slot);

// The pending order of a formation: retained from the order until the centre path search ends, then replaced by the
// members' own paths. The centre request is the lead's search request (it differs from the lead's own base request).
struct LandFormationOrder {
    EntityId lead{};
    LandPathRequest request;
    math::Vec2 heading{};   // the plan's forward unit vector; slots are placed against it until the path gives the arrival heading
    std::vector<LandFormationSlot> slots;
    friend bool operator==(const LandFormationOrder&, const LandFormationOrder&) = default;
};

} // namespace eawr::sim::tactical
