#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/units/galactic_types.hpp"
#include "eawr/assets/assets.hpp"
#include <span>
#include <string>

namespace eawr::campaign {
// Explicit original-type identity bindings supplied by setup, never tactical substitutions
// or invented catalog-index IDs. Bindings must be strictly ascending nonzero TypeId.
struct FleetTypeBinding { sim::galactic::TypeId id{}; std::string original_type; };
[[nodiscard]] core::Result<std::vector<sim::galactic::FleetTypeTraits>> build_fleet_type_traits(
    const units::GalacticTypeTable& types, std::span<const FleetTypeBinding> bindings);
// Object/ability services supply runtime eligibility and container kind. Authored
// templates cannot stand in for live contents or an active combat-skipping ability.
struct MovementTypeBinding {
    sim::galactic::TypeId id{};
    std::string original_type;
    bool hyperspace_eligible{}, squadron{}, fleet_container{}, skips_combat{};
    bool transport{};
};
struct MovementPlayerBinding { sim::galactic::PlayerId id{}; std::string faction; };
struct MovementInputs {
    std::vector<sim::galactic::MovementType> types;
    std::vector<sim::galactic::MovementPlayer> players;
    sim::math::Fixed base_mode_modifier;
};
// WGM-24/26/27/87/97: validated authored inputs, with provenance on omissions.
// Geometry/access/live contents are separate runtime-owner inputs to MovementSetup.
[[nodiscard]] core::Result<MovementInputs> build_movement_inputs(const units::GalacticTypeTable& types,
    const data::galactic::GalacticData& data, std::span<const MovementTypeBinding> objects,
    std::span<const MovementPlayerBinding> players);
// WGM-28/115: fresh zero-facing planets use collision bounds in reference pose,
// times their effective positive Scale_Factor. Corruption is fresh false.
[[nodiscard]] core::Result<sim::galactic::MovementPlanet> prepare_initial_movement_planet(
    sim::galactic::PlanetId, const assets::Model&, sim::math::Fixed scale_factor);
} // namespace eawr::campaign
