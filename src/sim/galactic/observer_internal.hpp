#pragma once

#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include "../math/wide.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace eawr::sim::galactic::observer_detail {
// WGUI-24/101: event-time planet-self transfer. Zero old/new selects insertion/removal.
void planet_spy_transition(ObserverSetup&, std::span<const Planet>, PlanetId,
    PlayerId old_owner, PlayerId new_owner, std::uint64_t frame,
    std::span<const Fleet>, const MovementSetup*, std::span<const CensusObject>);
template<class T, class Key, class Getter> const T* find(const std::vector<T>& rows, Key key, Getter get) {
    const auto it = std::lower_bound(rows.begin(), rows.end(), key, [&](const T& row, Key value) { return get(row) < value; });
    return it != rows.end() && get(*it) == key ? &*it : nullptr;
}
inline bool allied(const ObserverPlayer& player, PlayerId other) {
    return other == player.player || std::binary_search(player.allies.begin(), player.allies.end(), other);
}
// WGUI-04: compare exact squared raw coordinates, never a rounded length.
inline bool nearby(const math::Vec3& a, const math::Vec3& b, math::Fixed range) {
    if (range == math::Fixed{}) return true;
    math::detail::UInt192 sum;
    const auto add = [&](math::Fixed x, math::Fixed y) {
        const auto delta = x >= y ? static_cast<std::uint64_t>(x.raw()) - static_cast<std::uint64_t>(y.raw())
            : static_cast<std::uint64_t>(y.raw()) - static_cast<std::uint64_t>(x.raw());
        (void)math::detail::add_magnitude(sum, math::detail::multiply_u64(delta, delta));
    };
    add(a.x, b.x); add(a.y, b.y); add(a.z, b.z);
    const auto r = static_cast<std::uint64_t>(range.raw());
    return math::detail::compare(sum, math::detail::multiply_u64(r, r)) < 0;
}
struct Presence { bool fleet{}, reveal_force{}, superweapon{}; };
struct PresenceTable : std::map<std::pair<PlayerId, PlanetId>, Presence> {
    std::set<PlanetId> superweapon_planets;
};
inline PresenceTable presence(const ObserverSetup& inputs, std::span<const Fleet> fleets,
    const MovementSetup* movement = nullptr, std::span<const CensusObject> actors = {}) {
    PresenceTable table;
    for (const auto& fleet : fleets) {
        if (fleet.motion.transit) continue;
        auto& row = table[{fleet.owner, fleet.orbit}]; row.fleet = true;
        if (!actors.empty()) {
            const auto skip = fleet_combat_skip(fleet, actors, movement);
            row.reveal_force = row.reveal_force || (skip && !skip.value());
        }
        const auto member = [&](auto&& self, const FleetMember& value, std::size_t depth) -> void {
            if (depth > 32) return; // movement admission validates the live container depth
            const auto* children = movement ? find(movement->contents, value.id, [](const auto& c) { return c.container; }) : nullptr;
            if (children) { for (const auto& child : children->children) self(self, child, depth + 1); return; }
            const auto* traits = find(inputs.types, value.type, [](const auto& t) { return t.type; });
            if (!traits) return; // unresolved runtime type stays a named producer seam
            if (actors.empty()) row.reveal_force = row.reveal_force || (!traits->stealth_object && !traits->stealth_hero);
            row.superweapon = row.superweapon || traits->superweapon;
            if (traits->superweapon) table.superweapon_planets.insert(fleet.orbit);
        };
        for (const auto& value : fleet.members) member(member, value, 0);
    }
    return table;
}
inline IntelligenceContext context(const ObserverSetup& inputs, ObserverViewContext view, const ObserverPlayer& player,
    const ObserverPlanet& planet, const ObserverRecord& record, const PresenceTable& table) {
    IntelligenceContext c;
    c.observer = player.player;
    c.local_player = view.player ? view.player : inputs.solo_reference_human ? inputs.solo_reference_human : player.player;
    c.allegiance = planet.allegiance; c.target_owner = planet.target_owner;
    c.space_owner = planet.space_owner; c.ground_owner = planet.ground_owner;
    c.corruption_benefit = player.benefits_from_corruption; c.corrupted = planet.corrupted;
    c.space_stealth = planet.space_stealth; c.ground_stealth = planet.ground_stealth;
    c.allied_space_owner = allied(player, planet.space_owner); c.allied_ground_owner = allied(player, planet.ground_owner);
    c.own_orbit = table.contains({player.player, planet.planet});
    for (const auto& transport : planet.landed_transports) if (transport.id && !transport.delete_pending) {
        ++c.landed_transport_count;
        c.own_landed |= transport.owner == player.player;
    }
    c.attached_superweapon = table.superweapon_planets.contains(planet.planet);
    c.space_level = planet.space_level; c.ground_level = planet.ground_level;
    c.minimum_visible_level = player.minimum_visible_base_level; c.mask = record.mask;
    return c;
}
inline void refresh(ObserverRecord& record, const ObserverPlayer& player, const ObserverPlanet& planet,
    const PresenceTable& table, std::uint64_t frame, bool clear_local = true) {
    // WGUI-99: an ordinary local refresh clears before recomputation; a direct
    // recomputation retains the prior bits and caches CURRENT actual values on loss.
    if (clear_local && !player.ai) { record.mask = 0; record.apparent_control = {}; record.apparent_owner = -1; }
    const auto previous = record.mask;
    record.refresh_frame = frame;
    std::erase_if(record.sources, [&](const auto& s) { return s.final_frame < frame; }); // WGUI-23, equality retained
    record.mask = planet.target_owner == player.player ? (player.ai ? all_intelligence : owner_intelligence) : player.default_intelligence;
    if (player.ai && planet.target_owner != player.player) {
        if (table.contains({player.player, planet.planet})) record.mask |= bit(Intelligence::fleet_presence) | bit(Intelligence::fleet_count) | bit(Intelligence::fleet_contents);
        if (std::any_of(planet.landed_transports.begin(), planet.landed_transports.end(), [&](const auto& t) { return t.id && !t.delete_pending && t.owner == player.player; }))
            record.mask |= bit(Intelligence::ownership) | bit(Intelligence::fleet_presence) | bit(Intelligence::ground_count) | bit(Intelligence::ground_contents);
    }
    for (const auto& source : record.sources) record.mask |= source.mask;
    // WGUI-99 explicitly samples CURRENT actual control/controller at bit loss,
    // even if that controller was never observed; no prior-observation memory is
    // substituted. WGUI-100 consumes the resulting apparent value separately.
    if ((previous & bit(Intelligence::ownership)) && !(record.mask & bit(Intelligence::ownership))) record.apparent_owner = planet.current_controller;
    if ((previous & bit(Intelligence::political_control)) && !(record.mask & bit(Intelligence::political_control))) record.apparent_control = planet.political_control;
}
} // namespace eawr::sim::galactic::observer_detail
