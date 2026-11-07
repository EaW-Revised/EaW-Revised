#pragma once

#include "eawr/sim/galactic/fleet_intelligence.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {

// WGUI-72..74: the fleet projection leaves an enemy group untyped (a bare count, no icon) when its type has no
// display trait, so a live campaign supplies one trait per type from the authored object. `Art` maps an object name
// to a row with `hero`, `named_hero`, `force_sensitive` and an optional `max_health` (Tactical_Health).
// `damage` carries the authored maximum health as the ranking key of the strongest-ship rule (not a current value).
template <class Art>
[[nodiscard]] std::vector<sim::galactic::FleetDisplayType> live_fleet_display_traits(
    const std::map<sim::galactic::TypeId, std::string>& names, const Art& art) {
    std::vector<sim::galactic::FleetDisplayType> traits;
    traits.reserve(names.size());
    for (const auto& [type, name] : names) {
        const auto row = art.find(name);
        if (row == art.end()) continue;
        sim::galactic::FleetDisplayType trait;
        trait.type = type; trait.hero = row->second.hero; trait.named_hero = row->second.named_hero;
        trait.force_sensitive = row->second.force_sensitive;
        if (row->second.max_health)
            trait.damage = sim::math::Fixed::from_raw(static_cast<std::int64_t>(
                std::llround(static_cast<double>(*row->second.max_health) * static_cast<double>(sim::math::Fixed::scale))));
        traits.push_back(trait);
    }
    return traits;
}

// GPD-14: the published fleet-control traits classify live transport members.
inline void apply_fleet_transport_traits(std::vector<sim::galactic::FleetDisplayType>& display,
    const std::vector<sim::galactic::FleetTypeTraits>& types) {
    for (auto& row : display) {
        const auto found = std::lower_bound(types.begin(), types.end(), row.type,
            [](const auto& type, const auto id) { return type.type < id; });
        if (found != types.end() && found->type == row.type) row.transport = found->transport;
    }
}

// The projection can disclose a local member without a display row. Resolve only
// those already disclosed local representatives against the authoritative fleet type.
inline void apply_local_transport_traits(std::vector<sim::galactic::ObservedFleet>& cards,
    std::span<const sim::galactic::Fleet> fleets, const std::vector<sim::galactic::FleetTypeTraits>& types) {
    std::map<sim::EntityId, sim::galactic::TypeId> local_members;
    const auto local = std::find_if(cards.begin(), cards.end(), [](const auto& card) { return card.local; });
    if (local == cards.end()) return;
    for (const auto& fleet : fleets) if (fleet.owner == local->owner)
        for (const auto& member : fleet.members) local_members.emplace(member.id, member.type);
    for (auto& card : cards) if (card.local) for (auto& group : card.groups) {
        if (!group.representative) continue;
        const auto member = local_members.find(*group.representative);
        if (member == local_members.end()) continue;
        const auto type = std::lower_bound(types.begin(), types.end(), member->second,
            [](const auto& row, const auto id) { return row.type < id; });
        if (type != types.end() && type->type == member->second) group.transport = type->transport;
    }
}

} // namespace eawr::viewer::galactic
