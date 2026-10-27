#include "eawr/presentation/ui/fleet_pathfinder.hpp"
#include <algorithm>

namespace eawr::presentation::ui {
void separate_pathfinder(sim::galactic::ObservedFleet& card, const sim::galactic::GalacticSnapshot& snapshot,
    const sim::EntityId selected, const std::span<const std::pair<sim::EntityId, std::int64_t>> indices,
    const std::span<const std::uint8_t> slots) {
    if (!selected || !card.local) return;
    const auto key = [&](const sim::galactic::FleetMember& member) {
        auto type = member.type;
        const auto actors = snapshot.actor_records();
        const auto actor = std::lower_bound(actors.begin(), actors.end(), member.id,
            [](const auto& row, auto id) { return row.id < id; });
        if (actor != actors.end() && actor->id == member.id && actor->original_type) type = actor->original_type;
        std::optional<std::int64_t> index;
        const auto remembered = std::lower_bound(indices.begin(), indices.end(), member.id,
            [](const auto& row, auto id) { return row.first < id; });
        if (remembered != indices.end() && remembered->first == member.id) index = remembered->second;
        return std::pair{type, index};
    };
    std::optional<std::pair<sim::galactic::TypeId, std::optional<std::int64_t>>> selected_key;
    for (const auto& fleet : snapshot.fleets()) for (const auto& member : fleet.members)
        if (member.id == selected) selected_key = key(member);
    if (!selected_key) return;
    for (auto& group : card.groups) {
        // Mixed orbit panels fold by type across slots, independently of each source GUI index.
        if (group.type != selected_key->first || (card.orbit_slot && group.gui_index != selected_key->second) || !group.count) continue;
        --group.count;
        if (group.representative == selected && group.count) {
            for (const auto& fleet : snapshot.fleets()) {
                if (fleet.owner != card.owner || fleet.orbit != card.planet || fleet.motion.transit
                    || (card.orbit_slot && fleet.orbit_slot != *card.orbit_slot)
                    || (!slots.empty() && std::find(slots.begin(), slots.end(), fleet.orbit_slot) == slots.end())) continue;
                for (const auto& member : fleet.members) if (member.id != selected
                    && (card.orbit_slot ? key(member) == *selected_key : key(member).first == selected_key->first)) {
                    group.representative = member.id; group.fleet = fleet.id; return;
                }
            }
        }
        break;
    }
    std::erase_if(card.groups, [](const auto& group) { return group.count == 0; });
}

std::optional<FleetPathfinder> fleet_pathfinder(const sim::galactic::GalacticSnapshot& snapshot,
    const sim::galactic::PlayerId player, const sim::galactic::PlanetId planet,
    const std::span<const std::uint8_t> slots) {
    for (const auto& fleet : snapshot.fleets()) {
        if (fleet.owner != player || fleet.orbit != planet || fleet.motion.transit
            || std::find(slots.begin(), slots.end(), fleet.orbit_slot) == slots.end()) continue;
        for (const auto& member : fleet.members) if (member.pathfinder) return FleetPathfinder{fleet.id, member};
    }
    return std::nullopt;
}
std::optional<sim::galactic::SetFleetPathfinder> fleet_pathfinder_order(
    const sim::galactic::GalacticSnapshot& snapshot, const sim::SeatId seat,
    const sim::galactic::FleetId id, const sim::EntityId member, const bool system_hold, const bool pending_conflict) {
    namespace gc = sim::galactic;
    if (!snapshot.fleet_control()) return std::nullopt;
    const auto issuer = std::find_if(snapshot.seats().begin(), snapshot.seats().end(),
        [seat](const auto& row) { return row.seat_id == seat; });
    if (issuer == snapshot.seats().end() || !issuer->active || issuer->kind != gc::SeatKind::human) return std::nullopt;
    const auto& control = *snapshot.fleet_control();
    if (snapshot.clock().pause_depth && (system_hold || pending_conflict || control.pause.fleet_management == gc::PausedAdmission::refuse)) return std::nullopt;
    const auto humans = std::count_if(snapshot.seats().begin(), snapshot.seats().end(),
        [](const auto& row) { return row.kind == gc::SeatKind::human; });
    if (humans <= 1 && pending_conflict && (control.gates.queued_campaign_conflict || control.gates.active_speech)) return std::nullopt;
    for (const auto& fleet : snapshot.fleets()) if (fleet.id == id) {
        if (fleet.owner != issuer->player || !fleet.orbit || fleet.motion.transit) return std::nullopt;
        if (!member) return gc::SetFleetPathfinder{id, 0};
        for (const auto& row : fleet.members) if (row.id == member) {
            for (const auto& type : control.types) if (type.type == row.type && type.space_pathfinder)
                return gc::SetFleetPathfinder{id, member};
            return std::nullopt;
        }
    }
    return std::nullopt;
}
}
