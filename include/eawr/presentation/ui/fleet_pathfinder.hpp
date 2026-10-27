#pragma once

#include "eawr/sim/galactic/snapshot.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"

namespace eawr::presentation::ui {
struct FleetPathfinder final {
    sim::galactic::FleetId fleet{};
    sim::galactic::FleetMember member{};
};
// GHUD-R43/44: read the explicit selector from publication, never from a GUI cache.
[[nodiscard]] std::optional<FleetPathfinder> fleet_pathfinder(
    const sim::galactic::GalacticSnapshot&, sim::galactic::PlayerId,
    sim::galactic::PlanetId, std::span<const std::uint8_t> slots);
// WGT-06/40: mirror direct-member/type/seat admission; interaction supplies the effective system hold.
[[nodiscard]] std::optional<sim::galactic::SetFleetPathfinder> fleet_pathfinder_order(
    const sim::galactic::GalacticSnapshot&, sim::SeatId, sim::galactic::FleetId,
    sim::EntityId member, bool system_hold = false, bool pending_conflict = false);
// Remove the displayed selector from the ordinary cells; counts on the fleet header stay intact.
void separate_pathfinder(sim::galactic::ObservedFleet&, const sim::galactic::GalacticSnapshot&,
    sim::EntityId member, std::span<const std::pair<sim::EntityId, std::int64_t>> indices,
    std::span<const std::uint8_t> slots = {});
}
