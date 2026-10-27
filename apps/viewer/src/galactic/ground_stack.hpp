#pragma once
#include "ground_roster.hpp"
#include "fleet_stacks.hpp"

namespace eawr::viewer::galactic {
// GGS-01/02: the overview has one ground aggregate, distinct from orbit slots.
struct GroundStack {
    PlayerId owner{};
    std::size_t count{}, texture{};
    bool contents_known{}, allied{};
    std::vector<GroundCompany> companies;
    std::vector<FleetStackEntry> contents;
};
inline std::optional<GroundStack> ground_stack(const sim::galactic::GalacticSnapshot& snapshot,
    PlayerId local, ObjectId planet, std::size_t textures, bool empty_drop_target = false) {
    const auto roster = ground_roster(snapshot, local, planet);
    if (!roster.accessible || (!roster.contents_known && !roster.count_known) || !snapshot.observer()) return {};
    const auto& facts = snapshot.observer()->planets;
    const auto observed = std::find_if(facts.begin(), facts.end(),
        [planet](const auto& row) { return row.planet == planet; });
    if (observed == facts.end()) return {};
    const auto faction = [&](PlayerId owner) -> std::optional<sim::galactic::FactionId> {
        for (const auto& player : snapshot.players()) if (player.player_id == owner) return player.faction_id;
        return {};
    };
    const auto owner_faction = faction(observed->allegiance);
    if (!owner_faction) return {};
    GroundStack result;
    result.owner = observed->allegiance; result.allied = roster.allied;
    result.contents_known = roster.contents_known;
    for (const auto& transport : observed->landed_transports) {
        if (!transport.id || transport.delete_pending || faction(transport.owner) != owner_faction) continue;
        ++result.count;
    }
    if (result.contents_known) for (const auto& company : roster.companies) {
        result.companies.push_back(company);
        const auto group = std::find_if(result.contents.begin(), result.contents.end(),
            [&](const auto& row) { return row.type == company.type; });
        if (group == result.contents.end()) result.contents.push_back({company.type, 1});
        else ++group->count;
    }
    if (!result.count && !(empty_drop_target && result.allied)) return {};
    result.texture = stack_texture_index(result.count, textures);
    if (!result.contents_known && result.count) result.contents.push_back({{}, result.count});
    return result;
}
// GGS-05: an empty allied surface becomes a receiver while ground units are held
// (owner-specified; retail visual capture pending). Orbit drops retain their
// same-planet landing admission, including mixed stacks with a loaded transport.
inline std::optional<GroundStack> ground_drop_stack(const sim::galactic::GalacticSnapshot& snapshot,
    PlayerId local, ObjectId planet, std::size_t textures, ObjectId fleet,
    std::span<const ObjectId> stack = {}, bool surface_units_held = false,
    std::span<const sim::galactic::FleetMemberRef> members = {}) {
    if (!fleet) return ground_stack(snapshot, local, planet, textures, surface_units_held);
    const auto fleets = snapshot.fleets();
    const auto source = std::find_if(fleets.begin(), fleets.end(),
        [fleet](const auto& row) { return row.id == fleet; });
    if (source == fleets.end() || source->orbit != planet) return ground_stack(snapshot, local, planet, textures);
    bool selected_ground = members.empty();
    if (!members.empty() && snapshot.control() && snapshot.fleet_control()) for (const auto& ref : members) {
        const auto parent = std::find_if(fleets.begin(), fleets.end(),
            [&](const auto& row) { return row.id == ref.fleet && row.owner == local && row.orbit == planet; });
        if (parent == fleets.end()) continue;
        const auto unit = std::find_if(parent->members.begin(), parent->members.end(),
            [&](const auto& row) { return row.id == ref.member; });
        if (unit == parent->members.end()) continue;
        const auto& types = snapshot.fleet_control()->types;
        const auto type = std::find_if(types.begin(), types.end(), [&](const auto& row) { return row.type == unit->type; });
        if (type == types.end() || !type->transport) continue;
        const auto capable = sim::galactic::has_ground_capability(*snapshot.control(), *unit, snapshot.movement());
        if (capable && capable.value()) selected_ground = true;
    }
    const bool landing = selected_ground && source != fleets.end() && source->orbit == planet
        && fleet_can_land(snapshot, local, fleet, stack);
    return ground_stack(snapshot, local, planet, textures, surface_units_held || landing);
}
// GGS-04: only observer-admitted local companies enter a seat command.
inline std::vector<ObjectId> ground_stack_units(const GroundStack& stack) {
    std::vector<ObjectId> result;
    if (stack.allied && stack.contents_known)
        for (const auto& company : stack.companies) if (company.enabled) result.push_back(company.id);
    return result;
}
inline std::optional<DropIntent> ground_stack_launch(const sim::galactic::GalacticSnapshot& snapshot,
    PlayerId local, ObjectId origin, std::span<const ObjectId> units, ObjectId target,
    std::uint8_t slot, std::vector<sim::galactic::PlanetId> path = {},
    std::optional<std::uint8_t> destination_slot = {}, bool land_on_arrival = false) {
    const auto stack = ground_stack(snapshot, local, origin, 0);
    if (!stack || units.empty() || slot > 2 || (target && (!path.empty() || slot))) return {};
    const auto live = ground_stack_units(*stack);
    for (std::size_t index = 0; index < units.size(); ++index)
        if (std::find(live.begin(), live.end(), units[index]) == live.end()
            || std::find(units.begin(), units.begin() + static_cast<std::ptrdiff_t>(index), units[index])
                != units.begin() + static_cast<std::ptrdiff_t>(index)) return {};
    if (target && std::none_of(snapshot.fleets().begin(), snapshot.fleets().end(), [&](const auto& fleet) {
            return fleet.id == target && fleet.owner == local && fleet.orbit == origin && !fleet.motion.transit;
        })) return {};
    DropIntent result{DropKind::launch, units.front(), target, origin, std::move(path), slot,
        std::vector<ObjectId>(units.begin(), units.end()), {}};
    result.destination_slot = destination_slot;
    if (land_on_arrival) {
        if (target || destination_slot || result.path.size() < 2) return {};
        const auto destination = ground_roster(snapshot, local, result.path.back());
        if (!destination.accessible) return {};
        result.land_on_arrival = true;
    }
    return result;
}
} // namespace eawr::viewer::galactic
