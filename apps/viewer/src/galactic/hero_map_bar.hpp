#pragma once

#include "map_input.hpp"
#include <map>

namespace eawr::viewer::galactic {

struct HeroMapEntry {
    sim::galactic::TypeId type{};
    ObjectId actor{}, planet{};
    friend bool operator==(const HeroMapEntry&, const HeroMapEntry&) = default;
};

// WGUI-89: live locally owned named heroes with icons and parents, once per type.
// Rank and scheduled-spawn publication remain unverified adapter seams: stable type
// order and live entries only, with no invented cooldown/respawn indicators.
template<class Art>
[[nodiscard]] std::vector<HeroMapEntry> collect_map_heroes(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local,
    const std::map<sim::galactic::TypeId, std::string>& names, const Art& art) {
    const auto hero = [&](sim::galactic::TypeId type, bool named_only) {
        const auto name = names.find(type);
        if (name == names.end()) return false;
        const auto row = art.find(name->second);
        return row != art.end() && (named_only ? row->second.named_hero : row->second.hero);
    };
    std::map<sim::galactic::TypeId, HeroMapEntry> unique;
    const auto actors = snapshot.actor_records();
    for (const auto& actor : actors) {
        if (actor.owner != local || actor.dead || actor.delete_pending || !actor.container
            || !hero(actor.current_type, true)) continue;
        const auto& look = art.at(names.at(actor.current_type));
        if (look.icon.empty()) continue;
        const auto parent = std::lower_bound(actors.begin(), actors.end(), actor.container,
            [](const auto& row, auto id) { return row.id < id; });
        if (parent != actors.end() && parent->id == actor.container && hero(parent->current_type, false)) continue;
        const auto planet = actor_containing_planet(snapshot, actor.id);
        if (!planet) continue; // Transit/unresolved containment is not a fabricated destination.
        unique.try_emplace(actor.current_type, HeroMapEntry{actor.current_type, actor.id, planet});
    }
    std::vector<HeroMapEntry> result;
    for (const auto& [type, entry] : unique) {
        static_cast<void>(type);
        result.push_back(entry);
        if (result.size() == 6) break; // WGUI-89 display limit.
    }
    return result;
}

} // namespace eawr::viewer::galactic
