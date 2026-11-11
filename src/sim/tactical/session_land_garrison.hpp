#pragma once

#include "session_land_internal.hpp"

#include <vector>

// The land session's garrison service (docs/behaviour/walks/land-combat.md, G-01..G-09): the order admission here, the named
// service seam after the movement and occupancy commit (TacticalSession::Impl::land_garrison_service) and the death-phase
// release of a destroyed host (Tick::land_garrison_deaths), all in session_land_garrison.cpp.
namespace eawr::sim::tactical::session_detail {

[[nodiscard]] const units::LandGarrisonData* land_garrison_data(const LandSession& land, TypeId type) noexcept;
[[nodiscard]] LiveUnit* land_live_unit(std::vector<LiveUnit>& live, EntityId id) noexcept;
[[nodiscard]] const LiveUnit* land_live_unit(const std::vector<LiveUnit>& live, EntityId id) noexcept;

// LC-101 (G-06): whether the occupants of the entry's host keep targeting and weapons. A structure keeps them; a transport
// disables them. A host that is a static root of the land session is a structure.
[[nodiscard]] bool land_garrison_keeps_weapons(const LandSession& land, const LandGarrisonEntry& entry) noexcept;

// LC-101..LC-103: the occupants of structures that fire. `ids` are the shooters (members of contained squads, ascending) and
// `squads` the contained squads they shoot for, each a retained shared target and scan frame like a squad in movement.
struct LandOccupantFire {
    struct Shooter { EntityId id{}; std::size_t squad{}; std::size_t slot{}; };  // squad: index into `squads`; slot: in entry.hidden
    struct Squad { EntityId entrant{}; std::vector<EntityId> roster; };
    std::vector<Shooter> ids;
    std::vector<Squad> squads;
};

// Copies the firing occupants into `roster` (kept ascending by entity id) as limbo shooters standing at their host. The hidden
// records stay where they are, so every reader of the containment sees the same state as before.
[[nodiscard]] LandOccupantFire land_garrison_splice_occupants(const LandSession& land, std::vector<LiveUnit>& roster);

// Moves the occupants' updated records (their weapon state) back into their entries and removes them from `roster`.
void land_garrison_restore_occupants(LandSession& land, std::vector<LiveUnit>& roster, const LandOccupantFire& fire);

// G-02: the entrants of a garrison order that the host accepts. Each accepted entrant gets an entry in the moving phase (slots
// are not tested here and pending entrants reserve none, G-03); a transport host is stopped (G-04). The caller turns the order
// into a move of the accepted squads to the host.
[[nodiscard]] std::vector<EntityId> admit_land_garrison(LandSession& land, std::vector<LiveUnit>& live, PlayerId issuer,
    const PlayerCommand& command, const LandGarrisonPayload& payload, std::uint64_t tick);

// G-07: the occupants of the named host (all of them, or one) that leave. They wait for the host to stand still. Returns how
// many were accepted.
[[nodiscard]] std::size_t admit_land_unload(LandSession& land, std::vector<LiveUnit>& live, PlayerId issuer,
    const PlayerCommand& command, const LandUnloadPayload& payload, std::uint64_t tick);

} // namespace eawr::sim::tactical::session_detail
