#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/core/load_note.hpp"

#include <span>
#include <map>

namespace eawr::sim::galactic {

struct ControlStep {
    PlanetControl state;
    core::LoadNoteBuffer notes; // one staged owner per planet, merged after phase join
    bool owner_changed{}, stop_production{}, degraded{};
    std::uint64_t route_visits{}, member_visits{}; // deterministic work, never state
};

[[nodiscard]] core::Result<void> validate_control_setup(const GalacticSetup& setup);
// GPC-01: sparse lifecycle writes; actor location zero means in transit/absent.
bool update_control_actor(PoliticalControlSetup&, const CensusObject&);
bool erase_control_actor(PoliticalControlSetup&, EntityId);
// WGM-106: an ordinary transport needs at least one direct payload combatant
// with positive current-type power that does not stay inside transports.
// No recursive capability or independent payload death filter is applied.
[[nodiscard]] core::Result<bool> has_ground_capability(const PoliticalControlSetup&,
    FleetMember, const MovementSetup*);
// WGM-C01/C03 and WGM-27: live occupation, independent of political ownership.
[[nodiscard]] core::Result<PlayerId> orbital_occupation(const PoliticalControlSetup*,
    PlanetId, std::span<const Fleet>, const MovementSetup*, std::span<const CensusObject> = {});
// WGM-65..74: one planet from immutable copied live presence and previous control.
[[nodiscard]] core::Result<ControlStep> calculate_planet_control(const PoliticalControlSetup& inputs,
    const ControlPlanet& planet, const PlanetControl& previous, std::span<const Fleet> orbit,
    const MovementSetup* movement, std::span<const Route> routes, std::uint64_t frame,
    std::span<const CensusObject> actors = {},
    const std::map<FleetId, core::Result<bool>>* live_skip = nullptr);

} // namespace eawr::sim::galactic
