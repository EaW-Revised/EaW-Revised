#pragma once

#include "eawr/sim/tactical/land_movement.hpp"

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
struct LandSessionContent {
    std::uint32_t clear_bit{};
    std::vector<LandCompanyBinding> companies;
    std::vector<LandOccupation> occupations;
    LandContactSettings contacts;
    // Type admission only; instance occupations are rasterized by the start adapter.
    std::vector<TypeId> static_objects;
};

} // namespace eawr::sim::tactical
