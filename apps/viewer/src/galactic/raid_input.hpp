#pragma once

#include "map_input.hpp"
#include "eawr/sim/galactic/raid.hpp"

namespace eawr::viewer::galactic {
[[nodiscard]] inline std::optional<DropIntent> raid_destination_intent(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local, ObjectId source,
    ObjectId target, std::span<const InputPlanet> planets) {
    const auto fleet = std::lower_bound(snapshot.fleets().begin(), snapshot.fleets().end(), source,
        [](const auto& row, auto id) { return row.id < id; });
    if (fleet == snapshot.fleets().end() || fleet->id != source || fleet->owner != local
        || !fleet->orbit || fleet->orbit == target || fleet->motion.transit
        || !snapshot.movement() || !snapshot.control()
        || !sim::galactic::qualifies_raid(*fleet, target, *snapshot.movement(), *snapshot.control(),
            snapshot.actor_records(), snapshot.planet_access())) return {};
    auto intent = fleet_destination_intent(snapshot, local, source, target, planets);
    if (!intent) return {};
    intent->kind = DropKind::ability;
    intent->ability = sim::galactic::MoveFleet{source, intent->path, {}, true};
    return intent;
}
} // namespace eawr::viewer::galactic
