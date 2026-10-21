#pragma once

#include "eawr/sim/galactic/types.hpp"
#include <algorithm>
#include <set>
#include <span>

namespace eawr::sim::galactic {
struct RaidForce {
    FactionId faction{};
    std::uint32_t categories{};
    bool hero{}, transport{}, stays_in_transport{};
};
// WGM-37: shared pure raid predicate, also used by campaign admission policy.
[[nodiscard]] inline bool raid_capable(std::span<const RaidForce> forces, const RaidRules& rules) {
    if (forces.empty() || (rules.required_faction && forces.front().faction != rules.required_faction)) return false;
    std::uint32_t heroes{}, limited{};
    for (const auto& row : forces) {
        if (row.transport && row.stays_in_transport) return false;
        if (row.hero && ++heroes > rules.max_heroes) return false;
        if ((row.categories & rules.free_categories) == 0) {
            if ((row.categories & rules.limited_categories) == 0 || ++limited > rules.max_limited) return false;
        }
    }
    return limited != 0;
}

// Resolve current owner facts before calling the shared predicate. No UI facts
// enter qualification; missing transport contents and story/access inputs refuse.
[[nodiscard]] inline bool qualifies_raid(const Fleet& fleet, PlanetId destination,
    const MovementSetup& movement, const PoliticalControlSetup& control,
    std::span<const CensusObject> actors, const PlanetAccessState* access = nullptr) {
    if (!movement.raid || fleet.members.empty()) return false;
    const auto planet = std::find_if(control.planets.begin(), control.planets.end(),
        [&](const auto& row) { return row.planet == destination; });
    const auto state = std::find_if(control.state.begin(), control.state.end(),
        [&](const auto& row) { return row.planet == destination; });
    const auto player = std::find_if(control.players.begin(), control.players.end(),
        [&](const auto& row) { return row.player == fleet.owner; });
    if (planet == control.planets.end() || !planet->surface_accessible || planet->destroyed
        || state == control.state.end() || !state->target_player || state->target_player == fleet.owner
        || player == control.players.end()) return false;
    const auto relation = std::find_if(control.relations.begin(), control.relations.end(), [&](const auto& row) {
        return row.player == fleet.owner && row.other == state->target_player;
    });
    if (relation == control.relations.end() || relation->allied) return false;
    if (access) {
        const auto row = std::find_if(access->planets.begin(), access->planets.end(),
            [&](const auto& p) { return p.planet == destination; });
        if (row == access->planets.end() || row->restricted) return false;
    }
    const auto movement_access = std::find_if(movement.access.begin(), movement.access.end(),
        [&](const auto& row) { return row.player == fleet.owner && row.planet == destination; });
    if (movement_access == movement.access.end() || !movement_access->accessible) return false;
    std::vector<RaidForce> forces;
    std::set<EntityId> seen;
    bool has_transport{};
    const auto inspect = [&](auto&& self, FleetMember member, bool carried) -> bool {
        if (!seen.insert(member.id).second) return false;
        if (!actors.empty()) {
            const auto actor = std::lower_bound(actors.begin(), actors.end(), member.id,
                [](const auto& row, auto id) { return row.id < id; });
            if (actor == actors.end() || actor->id != member.id || actor->dead || actor->delete_pending
                || actor->owner != fleet.owner || !actor->current_type) return false;
            member.type = actor->current_type;
        }
        const auto type = std::lower_bound(movement.types.begin(), movement.types.end(), member.type,
            [](const auto& row, auto id) { return row.type < id; });
        const auto policy = std::lower_bound(movement.raid->types.begin(), movement.raid->types.end(), member.type,
            [](const auto& row, auto id) { return row.type < id; });
        if (type == movement.types.end() || type->type != member.type
            || policy == movement.raid->types.end() || policy->type != member.type) return false;
        if ((carried || type->transport) && policy->stays_in_transport) return false;
        if (type->transport || type->fleet_container) {
            const auto children = std::lower_bound(movement.contents.begin(), movement.contents.end(), member.id,
                [](const auto& row, auto id) { return row.container < id; });
            if (children == movement.contents.end() || children->container != member.id) return false;
            if (type->transport) {
                has_transport = true;
                // WGM-37 classifies one transport by its first current payload;
                // a platoon's individual soldiers are not separate raid objects.
                for (auto child : children->children) {
                    if (!actors.empty()) {
                        const auto actor = std::lower_bound(actors.begin(), actors.end(), child.id,
                            [](const auto& row, auto id) { return row.id < id; });
                        if (actor == actors.end() || actor->id != child.id || actor->container != member.id) return false;
                        if (actor->dead || actor->delete_pending) continue;
                        if (!actor->current_type || actor->owner != fleet.owner) return false;
                        child.type = actor->current_type;
                    }
                    const auto payload = std::lower_bound(movement.raid->types.begin(), movement.raid->types.end(), child.type,
                        [](const auto& row, auto id) { return row.type < id; });
                    if (payload == movement.raid->types.end() || payload->type != child.type) return false;
                    bool hero = policy->hero || payload->hero;
                    if (!actors.empty()) {
                        const auto actor = std::lower_bound(actors.begin(), actors.end(), member.id,
                            [](const auto& row, auto id) { return row.id < id; });
                        const auto original = std::lower_bound(movement.raid->types.begin(), movement.raid->types.end(), actor->original_type,
                            [](const auto& row, auto id) { return row.type < id; });
                        if (original != movement.raid->types.end() && original->type == actor->original_type) hero = hero || original->hero;
                    }
                    forces.push_back({player->faction, payload->categories, hero, true, payload->stays_in_transport});
                    return true;
                }
                return false;
            }
            if (children->children.empty()) return false;
            for (const auto& child : children->children) if (!self(self, child, carried)) return false;
            return true;
        }
        forces.push_back({player->faction, policy->categories, policy->hero, carried, policy->stays_in_transport});
        return true;
    };
    for (const auto& member : fleet.members) if (!inspect(inspect, member, false)) return false;
    return has_transport && raid_capable(forces, movement.raid->rules);
}
} // namespace eawr::sim::galactic
