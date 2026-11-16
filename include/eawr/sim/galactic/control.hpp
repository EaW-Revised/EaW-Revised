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
// GGS-11 / WGM-39: allied surface linking needs transport behavior, independently
// of ground-combat participation. A nonallied destination retains WGM-106.
[[nodiscard]] core::Result<bool> can_land_transport(const PoliticalControlSetup&,
    const FleetControlSetup&, PlayerId player, PlayerId planet_owner, FleetMember, const MovementSetup*);
// WGM-C01/C03 and WGM-27: live occupation, independent of political ownership.
[[nodiscard]] core::Result<PlayerId> orbital_occupation(const PoliticalControlSetup*,
    PlanetId, std::span<const Fleet>, const MovementSetup*, std::span<const CensusObject> = {});
// WGM-61..63 / WGM-99: where a player's orbital (surface false) or surface (surface true) retreat from `source`
// goes: the nearest planet by its tier order (orbital occupation, base ownership, neutral planet, any planet; the
// first two swap for a surface retreat), strict squared distance, later-created planet wins a tie, and `source`
// itself when no other planet exists. `planets` are ascending by identity.
[[nodiscard]] core::Result<PlanetId> select_retreat_destination(std::span<const Planet> planets,
    const PoliticalControlSetup* control, std::span<const Fleet> fleets, const MovementSetup* movement,
    std::span<const CensusObject> actors, PlanetId source, PlayerId owner, bool surface);
// WGM-66: each planet's production controller, derived from its live queues: the owner of the front entry of the ground queue, then of
// the space queue (the one in progress), none while a queue is idle. Planets are paired by identity. Returns whether any row changed.
[[nodiscard]] bool derive_production_controllers(PoliticalControlSetup&, const ProductionSetup&);
// WGM-65..74: one planet from immutable copied live presence and previous control.
[[nodiscard]] core::Result<ControlStep> calculate_planet_control(const PoliticalControlSetup& inputs,
    const ControlPlanet& planet, const PlanetControl& previous, std::span<const Fleet> orbit,
    const MovementSetup* movement, std::span<const Route> routes, std::uint64_t frame,
    std::span<const CensusObject> actors = {},
    const std::map<FleetId, core::Result<bool>>* live_skip = nullptr);

} // namespace eawr::sim::galactic
