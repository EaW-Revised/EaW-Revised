#include "eawr/sim/galactic/fleet_intelligence.hpp"

#include "observer_internal.hpp"

namespace eawr::sim::galactic {
namespace {
bool hero_known(const FleetDisplayType& type, const ObserverType& observer,
    const PlanetKnowledge& knowledge, bool allied) {
    if (allied || !type.hero) return true;
    // WGUI-74: categories are independent; force-sensitive permission is not
    // nested beneath the named-hero shortcut. Ordinary contents requires stealth
    // to be absent for each applicable category.
    const bool ordinary = knowledge.fleet_contents && !observer.stealth_object && !observer.stealth_hero;
    return (type.named_hero && (knowledge.major_stealth || ordinary))
        || (!type.named_hero && (knowledge.minor_stealth || ordinary))
        || (type.force_sensitive && (knowledge.force_sensitive || ordinary));
}
// WGUI-72 (#3079): the list groups by the UI's true type, the object's original
// type. A company in orbit is its transport member, whose actor record keeps the
// company as original type, so the card shows the company, not the transport.
// Without an actor record (standalone fixtures) the live member type stands.
TypeId true_type(const GalacticSnapshot& snapshot, const FleetMember& member) {
    const auto rows = snapshot.actor_records();
    const auto at = std::lower_bound(rows.begin(), rows.end(), member.id,
        [](const auto& row, auto key) { return row.id < key; });
    return at != rows.end() && at->id == member.id && at->original_type ? at->original_type : member.type;
}
// Flat sorted lookup tables replace the per-publication std::maps. Building one
// keeps the FIRST row of a duplicate key, as map::emplace did.
template<class Key, class Value> using FlatTable = std::vector<std::pair<Key, Value>>;
template<class Key, class Value> void seal(FlatTable<Key, Value>& table) {
    std::stable_sort(table.begin(), table.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    table.erase(std::unique(table.begin(), table.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), table.end());
}
template<class Key, class Value> auto flat_find(const FlatTable<Key, Value>& table, const std::type_identity_t<Key>& key) {
    const auto at = std::lower_bound(table.begin(), table.end(), key, [](const auto& row, const std::type_identity_t<Key>& k) { return row.first < k; });
    return at != table.end() && at->first == key ? at : table.end();
}
// Insert keeping the table sorted; false (and no change) when the key exists.
template<class Key, class Value> bool flat_emplace(FlatTable<Key, Value>& table, const std::type_identity_t<Key>& key, Value value) {
    const auto at = std::lower_bound(table.begin(), table.end(), key, [](const auto& row, const std::type_identity_t<Key>& k) { return row.first < k; });
    if (at != table.end() && at->first == key) return false;
    table.insert(at, {key, std::move(value)});
    return true;
}
// Group scan state of one published card, kept until every merged fleet of
// its slot has contributed members.
struct CardScan {
    std::size_t card{};
    bool allied{};
    const PlanetKnowledge* knowledge{};
    // One entry per published group, in group order: the (type, position) key,
    // the representative member and the fleet owning it. The list is short (at
    // most the group capacity), so lookups scan it.
    struct Entry { std::pair<TypeId, std::int64_t> key; const FleetMember* representative; FleetId owner; };
    std::vector<Entry> entries;
    bool full{};
};
}

FleetIntelligence observed_fleets(const GalacticSnapshot& snapshot,
    const PlayerId local_player, const std::span<const FleetDisplayType> traits,
    const std::size_t group_capacity, const std::span<const FleetDisplayVisibility> visibility,
    const std::function<std::optional<std::uint32_t>(TypeId)>& icon_rank,
    const std::span<const std::pair<EntityId, std::int64_t>> presentation_indices) {
    FleetIntelligence output;
    if (!snapshot.observer() || !local_player || (snapshot.view_player() && snapshot.view_player() != local_player)) return output;
    const auto& observer = *snapshot.observer();
    const auto* player = observer_detail::find(observer.players, local_player, [](const auto& row) { return row.player; });
    if (!player) return output;
    FlatTable<PlanetId, const ObservedPlanet*> planets;
    FlatTable<PlanetId, std::optional<bool>> sight;
    sight.reserve(visibility.size());
    for (const auto& row : visibility) sight.emplace_back(row.planet, row.current_visible);
    seal(sight);
    for (const auto& row : snapshot.observations()) {
        ++output.observation_rows;
        if (row.player != local_player) continue;
        const auto supplied = flat_find(sight, row.planet);
        // current_sight_provider (#1153/WGUI-06/28): a persistent discovery
        // flag/reveal counter is not the independent current visibility predicate.
        // Only an explicitly supplied true result admits existence; no fallback.
        const bool visible = supplied != sight.end() && supplied->second.value_or(false);
        if (visible) planets.emplace_back(row.planet, &row);
    }
    seal(planets);
    FlatTable<TypeId, const FleetDisplayType*> types;
    types.reserve(traits.size());
    for (const auto& row : traits) types.emplace_back(row.type, &row);
    seal(types);
    std::vector<std::pair<PlanetId, PlayerId>> presence;
    // One scan per published card; local fleets of one (planet, slot) share it.
    std::vector<CardScan> scans;
    scans.reserve(snapshot.fleets().size());
    output.cards.reserve(snapshot.fleets().size());
    FlatTable<std::pair<PlanetId, std::uint8_t>, std::size_t> slots;
    FlatTable<EntityId, std::int64_t> gui_indices;
    for (const auto& row : presentation_indices) if (row.second >= 0) gui_indices.emplace_back(row);
    seal(gui_indices);
    const auto scan_members = [&](ObservedFleet& card, CardScan& scan, const Fleet& fleet) {
        // GHUD-S76: size-awareness admits the stack before this scan. Rank
        // its image from the real members, independently of the masked group
        // list and its capacity. This never modifies authoritative state.
        const auto image = [&](auto&& self, const FleetMember& member, std::size_t depth) -> void {
            if (!icon_rank || depth > 32) return;
            ++output.members_examined;
            const auto* children = snapshot.movement() ? observer_detail::find(snapshot.movement()->contents,
                member.id, [](const auto& row) { return row.container; }) : nullptr;
            if (children && !children->children.empty()) {
                for (const auto& child : children->children) self(self, child, depth + 1);
                return;
            }
            const auto type = true_type(snapshot, member);
            const auto rank = icon_rank(type);
            if (!rank) return;
            const auto previous = card.overview_type ? icon_rank(*card.overview_type) : std::nullopt;
            if (!previous || *rank > *previous) card.overview_type = type;
        };
        for (const auto& member : fleet.members) image(image, member, 0);
        for (const auto& member : fleet.members) {
            if (scan.full) return;
            const auto type = true_type(snapshot, member);
            // GHUD-S87: an individually positioned unit may split from its type
            // stack. Hidden groups retain type-only grouping (no layout leak).
            const auto gui = flat_find(gui_indices, member.id);
            const auto position = scan.allied && gui != gui_indices.end() ? gui->second : std::int64_t{-1};
            const auto key = std::pair{type, position};
            auto index = std::find_if(scan.entries.begin(), scan.entries.end(), [&](const auto& entry) { return entry.key == key; });
            if (index == scan.entries.end()) {
                // WGUI-72: reaching a new-type limit ends the scan, including
                // later duplicates and later merged fleets. WGUI-U07 keeps the
                // supplied membership order (fleet order, then member order).
                if (card.groups.size() >= group_capacity) { scan.full = true; return; }
                scan.entries.push_back({key, &member, fleet.id});
                index = scan.entries.end() - 1;
                card.groups.emplace_back();
            }
            ++card.groups[static_cast<std::size_t>(index - scan.entries.begin())].count;
        }
    };
    for (const auto& fleet : snapshot.fleets()) {
        ++output.fleets_examined;
        if (fleet.motion.transit || !fleet.id) continue;
        const auto planet = flat_find(planets, fleet.orbit);
        if (planet == planets.end()) continue;
        const auto& knowledge = planet->second->knowledge;
        const bool allied = observer_detail::allied(*player, fleet.owner);
        bool nonstealth = false, resolved = true;
        // WGUI-28/29: mixed fleets are nonstealth; all-stealth fleets remain
        // hidden from hostile orbit awareness. Unknown traits fail closed.
        const auto visit = [&](auto&& self, const FleetMember& member, std::size_t depth) -> void {
            ++output.members_examined;
            if (depth > 32) { resolved = false; return; }
            const auto* children = snapshot.movement() ? observer_detail::find(snapshot.movement()->contents,
                member.id, [](const auto& row) { return row.container; }) : nullptr;
            if (children) {
                for (const auto& child : children->children) self(self, child, depth + 1);
                return;
            }
            const auto* type = observer_detail::find(observer.types, member.type, [](const auto& row) { return row.type; });
            if (!type) { resolved = false; return; }
            nonstealth |= !type->stealth_object && !type->stealth_hero;
        };
        if (snapshot.actor_records().empty()) {
            // Standalone fixtures supply explicit observer facts without an actor owner.
            for (const auto& member : fleet.members) visit(visit, member, 0);
        } else {
            const auto skip = snapshot.fleet_combat_skip(fleet.id);
            resolved = bool(skip); nonstealth = skip && !skip.value();
        }
        const bool visible = allied || (resolved && nonstealth);
        if (visible && (allied || knowledge.fleet_presence)) presence.emplace_back(fleet.orbit, fleet.owner);
        if (!visible || (!allied && !knowledge.fleet_count)) continue;
        // WGEP-40/WGEP-66, GQP-11: fleets are never merged at placement, so the
        // slot's panel shows every local fleet of (planet, slot) as one card. A
        // local fleet is always allied: its contents and identity are known.
        const bool local = fleet.owner == local_player;
        const auto slot = std::pair{fleet.orbit, fleet.orbit_slot};
        const auto merged = local ? flat_find(slots, slot) : slots.end();
        if (merged != slots.end()) {
            auto& scan = scans[merged->second];
            auto& card = output.cards[scan.card];
            card.count += fleet.members.size();
            scan_members(card, scan, fleet);
            continue;
        }
        ObservedFleet card{fleet.id, fleet.orbit, fleet.owner, fleet.orbit_slot,
            local, fleet.members.size(), {}, {}};
        // WGUI-29/42: count awareness alone publishes no fleet identity/slot.
        if (!allied && !knowledge.fleet_contents) { card.fleet.reset(); card.orbit_slot.reset(); }
        scans.push_back({output.cards.size(), allied, &knowledge, {}, false});
        {
            const auto expected = std::min(group_capacity, fleet.members.size());
            scans.back().entries.reserve(expected); card.groups.reserve(expected);
        }
        if (local) flat_emplace(slots, slot, scans.size() - 1);
        scan_members(card, scans.back(), fleet);
        output.cards.push_back(std::move(card));
    }
    // Groups are classified once per card, after every merged fleet is scanned.
    for (const auto& scan : scans) {
        auto& card = output.cards[scan.card];
        const auto& entries = scan.entries;
        const auto& knowledge = *scan.knowledge;
        const bool allied = scan.allied;
        std::optional<std::size_t> strongest;
        auto greatest = math::Fixed::from_raw(-math::Fixed::scale);
        if (!allied && !knowledge.fleet_contents && knowledge.strongest_ship) {
            bool complete = true;
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto type = flat_find(types, entries[index].representative->type);
                if (type == types.end() || !type->second->damage) complete = false;
                if (type != types.end() && type->second->damage && *type->second->damage > greatest) {
                    greatest = *type->second->damage; strongest = index;
                }
            }
            if (!complete) strongest.reset();
        }
        for (std::size_t index = 0; index < card.groups.size(); ++index) {
            auto& group = card.groups[index];
            const auto& member = *entries[index].representative;
            const auto type = flat_find(types, member.type);
            const auto* observed_type = observer_detail::find(observer.types, member.type, [](const auto& row) { return row.type; });
            if (allied && type == types.end()) {
                // FleetMember is authoritative live direct membership. Its type
                // may be displayed to allies without classifying an enemy hero.
                group.type = true_type(snapshot, member); group.representative = member.id;
                group.fleet = entries[index].owner;
                if (const auto gui = flat_find(gui_indices, member.id); gui != gui_indices.end()) group.gui_index = gui->second;
                continue;
            }
            if (type == types.end() || !observed_type) continue; // missing hero metadata is not ordinary contents
            const auto& display = *type->second;
            const bool superweapon = observed_type->superweapon;
            const bool details = allied || knowledge.fleet_contents || strongest == index || superweapon;
            auto live_observer = *observed_type;
            if (!snapshot.actor_records().empty()) {
                const auto skip = snapshot.actor_combat_skip(member.id);
                if (!skip && !allied) continue;
                live_observer.stealth_object = skip && skip.value();
                live_observer.stealth_hero = display.hero && live_observer.stealth_object;
            }
            if (!details || !hero_known(display, live_observer, knowledge, allied)) continue;
            group.type = true_type(snapshot, member); group.representative = member.id;
            if (allied) if (const auto gui = flat_find(gui_indices, member.id); gui != gui_indices.end()) group.gui_index = gui->second;
            // Count-only knowledge names no fleet, as the card does (WGUI-29/42).
            if (allied || knowledge.fleet_contents) group.fleet = entries[index].owner;
            group.gui_slot = display.gui_slot == -1 ? 0 : display.gui_slot;
            group.transport = display.transport;
            group.strongest = strongest == index; group.superweapon = superweapon; group.hero = display.hero;
        }
    }
    std::sort(presence.begin(), presence.end());
    presence.erase(std::unique(presence.begin(), presence.end()), presence.end());
    for (const auto& [planet, owner] : presence) output.presence.push_back({planet, owner});
    return output;
}

} // namespace eawr::sim::galactic
