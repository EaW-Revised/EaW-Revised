#pragma once

#include "map_input.hpp"
#include "eawr/sim/galactic/hero_return.hpp"
#include <map>

namespace eawr::viewer::galactic {

struct HeroMapEntry {
    sim::galactic::TypeId type{};
    ObjectId actor{}, planet{};
    std::int64_t rank{25};
    bool scheduled{};
    std::uint64_t start_frame{}, deadline{};
    double completion{1.0};
    friend bool operator==(const HeroMapEntry&, const HeroMapEntry&) = default;
};

// WGUI-89: live locally owned named heroes with icons and parents, once per type.
// Visible portraits run from higher to lower authored rank (WGUI-89 capture).
// Equal ranks retain our canonical type order; retail tie order is unqualified.
// Scheduled rows come only from copied local owner publications.
template<class Art>
[[nodiscard]] std::vector<HeroMapEntry> collect_map_heroes(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local,
    const std::map<sim::galactic::TypeId, std::string>& names, const Art& art,
    std::span<const sim::galactic::HeroReturnView> returns = {}) {
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
        unique.try_emplace(actor.current_type, HeroMapEntry{actor.current_type, actor.id, planet, look.hero_rank});
    }
    for (const auto& row : returns) {
        if (!local || row.owner != local || !hero(row.type, true) || row.start_frame > row.deadline) continue;
        const auto& look = art.at(names.at(row.type));
        if (look.icon.empty()) continue;
        const auto frame = snapshot.clock().galactic_frame;
        double completion{};
        // WGUI-89: no wall time or inferred delay. Overdue retained placements reset to zero.
        if (frame >= row.start_frame && frame <= row.deadline) {
            completion = row.deadline == row.start_frame ? 1.0
                : static_cast<double>(frame - row.start_frame) / static_cast<double>(row.deadline - row.start_frame);
        }
        unique.try_emplace(row.type, HeroMapEntry{row.type, 0, row.planet, look.hero_rank,
            true, row.start_frame, row.deadline, completion});
    }
    std::vector<HeroMapEntry> result;
    for (const auto& [type, entry] : unique) {
        static_cast<void>(type);
        result.push_back(entry);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.rank > right.rank;
    });
    if (result.size() > 6) result.resize(6); // WGUI-89 display limit, after ranking.
    return result;
}

} // namespace eawr::viewer::galactic
