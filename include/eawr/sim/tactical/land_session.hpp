#pragma once

#include "eawr/sim/tactical/land_formation.hpp"
#include "eawr/sim/tactical/land_movement.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>
#include <utility>

namespace eawr::sim::tactical {

// Effective content bindings, in Company_Units order. Geometry and locomotion
// are supplied by the content adapter; the session has no stock-unit defaults.
struct LandMemberBinding {
    TypeId type{};
    LandLocomotionProfile locomotion;
    std::uint32_t collision_class{};
    std::optional<math::Fixed> formation_spacing;
    math::Fixed hard_x{}, hard_y{};
};
struct LandCompanyBinding {
    TypeId type{};
    units::LandCompany company;
    std::vector<LandMemberBinding> members;
    math::Fixed formation_spacing{};
    std::optional<math::Fixed> effective_team_max_speed;
    std::optional<math::Fixed> effective_team_type_max_speed;
};
// LC-44..51 (#5894): the rules a land battle's reinforcement runs on. All of it is authored or supplied data; no type is
// named here and nothing is derived from a stock value. `enabled == false` keeps the step exactly as it was.
struct LandReinforcementRules {
    struct Point { TypeId type{}; math::Fixed enabling_radius{}; };            // Reinforcement_Enabling_Radius > 0
    struct Capacity { TypeId type{}; std::int64_t value{}; };                  // Additional_Population_Capacity
    struct Prevention { TypeId type{}; math::Fixed radius{}; bool needs_power{}; }; // Reinforcement_Prevention_Radius > 0
    struct Cap { PlayerId player{}; std::int64_t value{}; };                   // faction curve at the lobby's player count
    bool enabled{};
    bool multiplayer{true};          // multiplayer tactical (LC-46) or a single land battle (LC-47)
    bool use_points{true};           // Use_Reinforcement_Points
    PlayerId defender{};             // the defending player of a single land battle; unused when multiplayer
    std::uint32_t max_ground_forces{};      // Max_Ground_Forces_On_Planet
    math::Fixed allow_fraction{};           // Allow_Reinforcement_Percentage_Normalized
    std::int64_t base_capacity{};           // the faction base capacity added to every owner's objects (LC-48)
    std::uint32_t delay_frames{};           // Skirmish_Reinforcement_Delay_Frames: creation to the start of the landing clip
    std::uint32_t landing_frames{};         // the landing clip length (named deviation: art data, not read)
    std::uint32_t unload_frames{};          // one unload step: 2 x (1 + deploy-time modifiers) seconds
    std::uint32_t capture_countdown_frames{}; // LC-52: Land_Capture_Allowed_Countdown_Seconds in frames; single land battles only
    std::vector<Point> points;              // strictly increasing type
    std::vector<Capacity> capacities;       // strictly increasing type
    std::vector<Prevention> preventions;    // strictly increasing type
    std::vector<Cap> caps;                  // strictly increasing player
    [[nodiscard]] const Point* point(TypeId type) const noexcept {
        const auto found = std::lower_bound(points.begin(), points.end(), type,
            [](const Point& row, const TypeId key) { return row.type < key; });
        return found != points.end() && found->type == type ? &*found : nullptr;
    }
    [[nodiscard]] std::int64_t capacity(TypeId type) const noexcept {
        const auto found = std::lower_bound(capacities.begin(), capacities.end(), type,
            [](const Capacity& row, const TypeId key) { return row.type < key; });
        return found != capacities.end() && found->type == type ? found->value : 0;
    }
    [[nodiscard]] const Prevention* prevention(TypeId type) const noexcept {
        const auto found = std::lower_bound(preventions.begin(), preventions.end(), type,
            [](const Prevention& row, const TypeId key) { return row.type < key; });
        return found != preventions.end() && found->type == type ? &*found : nullptr;
    }
    [[nodiscard]] std::optional<std::int64_t> cap(PlayerId player) const noexcept {
        const auto found = std::lower_bound(caps.begin(), caps.end(), player,
            [](const Cap& row, const PlayerId key) { return row.player < key; });
        if (found == caps.end() || found->player != player) return std::nullopt;
        return found->value;
    }
};

struct LandSessionContent {
    struct GarrisonBinding { TypeId type{}; units::LandGarrisonData data; };
    // Initial parent/child links are derived from the land start and bound by its content identity.
    std::vector<std::pair<EntityId, EntityId>> initial_pad_children;
    std::uint32_t clear_bit{};
    std::vector<LandCompanyBinding> companies;
    std::vector<GarrisonBinding> garrisons; // sorted by type
    std::vector<LandOccupation> occupations;
    LandContactSettings contacts;
    // LMF-02..LMF-05: the formation constants of the data (unauthored: group orders keep one destination per squad).
    LandFormationSettings formation;
    // LC-102: `Garrisoned_Max_Attack_Distance_Multiplier` of the data; unauthored: occupants keep their ordinary range.
    std::optional<math::Fixed> garrisoned_range_multiplier;
    // Type admission only; instance occupations are rasterized by the start adapter.
    std::vector<TypeId> static_objects;
    // LC-44..53: map objects with no obstacle geometry (reinforcement points): admitted as static roots without an
    // instance occupation. Strictly increasing; a type may not also be a static object.
    std::vector<TypeId> point_objects;
    LandReinforcementRules reinforcement;
};

} // namespace eawr::sim::tactical
