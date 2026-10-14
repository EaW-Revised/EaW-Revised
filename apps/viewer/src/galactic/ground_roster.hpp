#pragma once
#include "eawr/sim/galactic/observer.hpp"
#include "map_input.hpp"
#include "eawr/sim/galactic/production.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <vector>

namespace eawr::viewer::galactic {
struct GroundCompany {
    ObjectId id{};
    sim::galactic::TypeId type{};
    bool enabled{};
};
// GHUD-S68: one built ground special structure of the focused planet, in the planet's
// creation order (the structure row shows the list front to back).
struct GroundStructure {
    ObjectId id{};
    sim::galactic::TypeId type{};
    PlayerId owner{};
    bool sale_allowed{};
    bool sale_available{};
    sim::math::Fixed refund;
};
struct GroundRoster {
    bool accessible{}, contents_known{}, count_known{}, allied{};
    std::optional<std::size_t> count;
    std::vector<GroundCompany> companies;
    // GHUD-S68: the planet's current land special-structure capacity, whether the local player
    // may see the built structures (observer `structures` knowledge), and those structures.
    std::size_t structure_capacity{};
    bool structures_known{};
    std::vector<GroundStructure> structures;
};
// GHUD-S69: pad slot placement of landed companies, after the debug build's planet slot cleanup.
// `layout[slot]` is the company (index into the roster) shown on that pad. A company keeps its
// preferred slot when that is valid and not claimed earlier; every other company takes the first
// free slot, in roster order.
using PadLayout = std::vector<std::optional<std::size_t>>;
inline PadLayout place_pads(const std::vector<GroundCompany>& companies,
    const std::map<ObjectId, std::size_t>& preferred, const std::size_t slots) {
    PadLayout layout(slots);
    std::vector<std::size_t> pending;
    for (std::size_t index = 0; index < companies.size(); ++index) {
        const auto wanted = preferred.find(companies[index].id);
        if (wanted != preferred.end() && wanted->second < slots && !layout[wanted->second]) layout[wanted->second] = index;
        else pending.push_back(index);
    }
    for (const auto index : pending) {
        const auto free = std::find_if(layout.begin(), layout.end(), [](const auto& slot) { return !slot; });
        if (free == layout.end()) break; // More companies than pads: the surplus has no pad.
        *free = index;
    }
    return layout;
}
// Drop `company` on `target`: an empty pad takes it; an occupied pad pushes the neighbours towards
// the nearest free pad below the target first, else above it. False when no pad is free.
inline bool move_pad(PadLayout& layout, const std::size_t company, const std::size_t target) {
    if (target >= layout.size()) return false;
    for (auto& slot : layout) if (slot == company) slot.reset();
    if (!layout[target]) { layout[target] = company; return true; }
    for (std::size_t below = target; below-- > 0;) {
        if (layout[below]) continue;
        for (std::size_t at = below; at < target; ++at) layout[at] = layout[at + 1];
        layout[target] = company; return true;
    }
    for (std::size_t above = target + 1; above < layout.size(); ++above) {
        if (layout[above]) continue;
        for (std::size_t at = above; at > target; --at) layout[at] = layout[at - 1];
        layout[target] = company; return true;
    }
    return false;
}
// WGUI-41: every identity is admitted by the observer's live transport list.
// Control supplies a type only for that exact identity/owner. Missing provider
// rows never become raw control-list contents (#2263/#1156).
inline GroundRoster ground_roster(const sim::galactic::GalacticSnapshot& snapshot,
    PlayerId local, ObjectId planet) {
    GroundRoster result;
    if (!snapshot.observer() || !snapshot.control()) return result;
    const auto& observer = *snapshot.observer();
    const auto player = std::find_if(observer.players.begin(), observer.players.end(),
        [local](const auto& row) { return row.player == local; });
    const auto facts = std::find_if(observer.planets.begin(), observer.planets.end(),
        [planet](const auto& row) { return row.planet == planet; });
    const auto observations = snapshot.observations();
    const auto knowledge = std::find_if(observations.begin(), observations.end(),
        [=](const auto& row) { return row.player == local && row.planet == planet; });
    const auto& planets = snapshot.control()->planets;
    const auto presence = std::find_if(planets.begin(), planets.end(),
        [planet](const auto& row) { return row.planet == planet; });
    if (player == observer.players.end() || facts == observer.planets.end()
        || knowledge == observations.end() || !knowledge->discovered || presence == planets.end()) return result;
    // GHUD-S68 / WGUI-34: capacity is authored data; the filled list needs structure knowledge.
    if (const auto& production = snapshot.production()) {
        const auto row = std::find_if(production->planets.begin(), production->planets.end(),
            [planet](const auto& value) { return value.planet == planet; });
        if (row != production->planets.end()) {
            result.structure_capacity = static_cast<std::size_t>(std::max<std::int64_t>(row->ground_slots, 0));
            result.structures_known = knowledge->knowledge.structures;
            if (result.structures_known) for (const auto& object : row->objects)
                if (object.domain == sim::galactic::ProductionDomain::ground
                    && object.kind == sim::galactic::ProductionKind::structure)
                {
                    const auto type = std::lower_bound(row->offers.begin(), row->offers.end(), object.type,
                        [](const auto& value, auto id) { return value.type < id; });
                    const bool have_type = type != row->offers.end() && type->type == object.type;
                    const auto admission = sim::galactic::ground_sale_admission(*row, local, object.id);
                    result.structures.push_back({object.id, object.type, object.owner,
                        admission == sim::galactic::ProductionAdmission::allowed,
                        admission == sim::galactic::ProductionAdmission::allowed || admission == sim::galactic::ProductionAdmission::locked,
                        have_type ? sim::galactic::ground_sale_refund(object, *type) : sim::math::Fixed{}});
                }
        }
    }
    result.accessible = presence->surface_accessible && !presence->destroyed;
    if (!result.accessible) return result;
    const auto allied = [](const auto& players, PlayerId a, PlayerId b) {
        if (a == b) return true;
        const auto p = std::find_if(players.begin(), players.end(), [a](const auto& row) { return row.player == a; });
        return p != players.end() && std::find(p->allies.begin(), p->allies.end(), b) != p->allies.end();
    };
    result.allied = allied(observer.players, local, facts->allegiance);
    result.contents_known = knowledge->knowledge.ground_contents;
    result.count_known = knowledge->knowledge.ground_count;
    if (result.count_known) result.count = static_cast<std::size_t>(std::count_if(
        facts->landed_transports.begin(), facts->landed_transports.end(),
        [](const auto& row) { return row.id && !row.delete_pending; }));
    if (!result.contents_known) return result;
    for (const auto& transport : facts->landed_transports) {
        if (!transport.id || transport.delete_pending || !allied(observer.players, facts->allegiance, transport.owner)) continue;
        const auto row = std::find_if(presence->landed.begin(), presence->landed.end(),
            [&](const auto& force) { return force.id == transport.id && force.owner == transport.owner; });
        if (row != presence->landed.end()) {
            // WGM-133 records the authored company type on its runtime transport.
            // Only this observer-admitted identity may supply its display type.
            const auto actors = snapshot.actor_records();
            const auto actor = std::lower_bound(actors.begin(), actors.end(), row->id,
                [](const auto& value, auto id) { return value.id < id; });
            const auto display_type = actor != actors.end() && actor->id == row->id && actor->owner == row->owner
                ? actor->original_type : row->type;
            result.companies.push_back({row->id, display_type, row->owner == local});
        }
    }
    return result;
}
} // namespace eawr::viewer::galactic
