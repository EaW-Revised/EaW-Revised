#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <span>
#include <map>

namespace eawr::sim::galactic {

struct ControlStep {
    PlanetControl state;
    bool owner_changed{}, stop_production{};
    std::uint64_t route_visits{}, member_visits{}; // deterministic work, never state
};

[[nodiscard]] core::Result<void> validate_control_setup(const GalacticSetup& setup);
// WGM-106: an ordinary transport needs at least one direct payload combatant
// with positive current-type power that does not stay inside transports.
// No recursive capability or independent payload death filter is applied.
[[nodiscard]] core::Result<bool> has_ground_capability(const PoliticalControlSetup&,
    FleetMember, const MovementSetup*);
// WGM-65..74: one planet from immutable copied live presence and previous control.
[[nodiscard]] core::Result<ControlStep> calculate_planet_control(const PoliticalControlSetup& inputs,
    const ControlPlanet& planet, const PlanetControl& previous, std::span<const Fleet> orbit,
    const MovementSetup* movement, std::span<const Route> routes, std::uint64_t frame,
    std::span<const CensusObject> actors = {},
    const std::map<FleetId, core::Result<bool>>* live_skip = nullptr);

} // namespace eawr::sim::galactic
