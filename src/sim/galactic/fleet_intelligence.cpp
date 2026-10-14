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
// Group scan state of one published card, kept until every merged fleet of
// its slot has contributed members.
struct CardScan {
    std::size_t card{};
    bool allied{};
    const PlanetKnowledge* knowledge{};
    std::map<std::pair<TypeId, std::int64_t>, std::size_t> indices;
    std::vector<const FleetMember*> representatives;
    std::vector<FleetId> owners; // fleet owning each group's representative
    bool full{};
};
}

FleetIntelligence observed_fleets(const GalacticSnapshot& snapshot,
    const PlayerId local_player, const std::span<const FleetDisplayType> traits,
    const std::size_t group_capacity, const std::span<const FleetDisplayVisibility> visibility,
    const std::function<std::optional<std::uint32_t>(TypeId)>& icon_rank,
    const std::span<const std::pair<EntityId, std::int64_t>> presentation_indices) {
    FleetIntelligence output;
    if (!snapshot.observer() || snapshot.observer()->local_player != local_player) return output;
    const auto& observer = *snapshot.observer();
    const auto* player = observer_detail::find(observer.players, local_player, [](const auto& row) { return row.player; });
    if (!player) return output;
    std::map<PlanetId, const ObservedPlanet*> planets;
    std::map<PlanetId, std::optional<bool>> sight;
    for (const auto& row : visibility) sight.emplace(row.planet, row.current_visible);
    for (const auto& row : snapshot.observations()) {
        ++output.observation_rows;
        if (row.player != local_player) continue;
        const auto supplied = sight.find(row.planet);
        // current_sight_provider (#1153/WGUI-06/28): a persistent discovery
        // flag/reveal counter is not the independent current visibility predicate.
        // Only an explicitly supplied true result admits existence; no fallback.
        const bool visible = supplied != sight.end() && supplied->second.value_or(false);
        if (visible) planets.emplace(row.planet, &row);
    }
    std::map<TypeId, const FleetDisplayType*> types;
    for (const auto& row : traits) types.emplace(row.type, &row);
    std::set<std::pair<PlanetId, PlayerId>> presence;
    // One scan per published card; local fleets of one (planet, slot) share it.
    std::vector<CardScan> scans;
    std::map<std::pair<PlanetId, std::uint8_t>, std::size_t> slots;
    std::map<EntityId, std::int64_t> gui_indices;
    for (const auto& row : presentation_indices) if (row.second >= 0) gui_indices.emplace(row);
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
            const auto gui = gui_indices.find(member.id);
            const auto position = scan.allied && gui != gui_indices.end() ? gui->second : -1;
            const auto key = std::pair{type, position};
            auto index = scan.indices.find(key);
            if (index == scan.indices.end()) {
                // WGUI-72: reaching a new-type limit ends the scan, including
                // later duplicates and later merged fleets. WGUI-U07 keeps the
                // supplied membership order (fleet order, then member order).
                if (card.groups.size() >= group_capacity) { scan.full = true; return; }
                index = scan.indices.emplace(key, card.groups.size()).first;
                card.groups.emplace_back();
                scan.representatives.push_back(&member);
                scan.owners.push_back(fleet.id);
            }
            ++card.groups[index->second].count;
        }
    };
    for (const auto& fleet : snapshot.fleets()) {
        ++output.fleets_examined;
        if (fleet.motion.transit || !fleet.id) continue;
        const auto planet = planets.find(fleet.orbit);
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
        if (visible && (allied || knowledge.fleet_presence)) presence.emplace(fleet.orbit, fleet.owner);
        if (!visible || (!allied && !knowledge.fleet_count)) continue;
        // WGEP-40/WGEP-66, GQP-11: fleets are never merged at placement, so the
        // slot's panel shows every local fleet of (planet, slot) as one card. A
        // local fleet is always allied: its contents and identity are known.
        const bool local = fleet.owner == local_player;
        const auto slot = std::pair{fleet.orbit, fleet.orbit_slot};
        const auto merged = local ? slots.find(slot) : slots.end();
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
        scans.push_back({output.cards.size(), allied, &knowledge, {}, {}, {}, false});
        if (local) slots.emplace(slot, scans.size() - 1);
        scan_members(card, scans.back(), fleet);
        output.cards.push_back(std::move(card));
    }
    // Groups are classified once per card, after every merged fleet is scanned.
    for (const auto& scan : scans) {
        auto& card = output.cards[scan.card];
        const auto& representatives = scan.representatives;
        const auto& knowledge = *scan.knowledge;
        const bool allied = scan.allied;
        std::optional<std::size_t> strongest;
        auto greatest = math::Fixed::from_raw(-math::Fixed::scale);
        if (!allied && !knowledge.fleet_contents && knowledge.strongest_ship) {
            bool complete = true;
            for (std::size_t index = 0; index < representatives.size(); ++index) {
                const auto type = types.find(representatives[index]->type);
                if (type == types.end() || !type->second->damage) complete = false;
                if (type != types.end() && type->second->damage && *type->second->damage > greatest) {
                    greatest = *type->second->damage; strongest = index;
                }
            }
            if (!complete) strongest.reset();
        }
        for (std::size_t index = 0; index < card.groups.size(); ++index) {
            auto& group = card.groups[index];
            const auto& member = *representatives[index];
            const auto type = types.find(member.type);
            const auto* observed_type = observer_detail::find(observer.types, member.type, [](const auto& row) { return row.type; });
            if (allied && type == types.end()) {
                // FleetMember is authoritative live direct membership. Its type
                // may be displayed to allies without classifying an enemy hero.
                group.type = true_type(snapshot, member); group.representative = member.id;
                group.fleet = scan.owners[index];
                if (const auto gui = gui_indices.find(member.id); gui != gui_indices.end()) group.gui_index = gui->second;
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
            if (allied) if (const auto gui = gui_indices.find(member.id); gui != gui_indices.end()) group.gui_index = gui->second;
            // Count-only knowledge names no fleet, as the card does (WGUI-29/42).
            if (allied || knowledge.fleet_contents) group.fleet = scan.owners[index];
            group.gui_slot = display.gui_slot == -1 ? 0 : display.gui_slot;
            group.transport = display.transport;
            group.strongest = strongest == index; group.superweapon = superweapon;
        }
    }
    for (const auto& [planet, owner] : presence) output.presence.push_back({planet, owner});
    return output;
}

} // namespace eawr::sim::galactic
