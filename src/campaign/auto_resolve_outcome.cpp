#include "eawr/campaign/auto_resolve_outcome.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace eawr::campaign {
namespace {
template <typename T> core::Result<T> fail(std::string message) {
    core::Diagnostic error; error.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
    error.message = std::move(message); return core::Result<T>::failure(std::move(error));
}
bool valid(const AutoResolveOutcomeApplication& a) {
    if (!a.planet || a.planet_type.empty() || !a.result.conflict || !a.invader || !a.defender || a.invader == a.defender
        || (a.result.winner != a.invader && a.result.winner != a.defender)
        || (a.result.retreat_player && (*a.result.retreat_player == a.result.winner
            || (*a.result.retreat_player != a.invader && *a.result.retreat_player != a.defender)))
        || !a.result.reserved_health.empty() || !a.result.credits.empty() || a.result.corruption_success
        || !std::is_sorted(a.destroyed.begin(), a.destroyed.end())
        || std::adjacent_find(a.destroyed.begin(), a.destroyed.end()) != a.destroyed.end()
        || (!a.destroyed.empty() && a.destroyed.front() == 0)
        || a.destroyed.size() > sim::galactic::galactic_max_objects || a.deaths.size() > sim::galactic::galactic_max_objects
        || a.result.forces.size() > sim::galactic::galactic_max_objects || a.record.size() > sim::galactic::galactic_max_objects
        || a.learning.size() > sim::galactic::galactic_max_objects || a.land_invaders.size() > sim::galactic::galactic_max_objects
        || a.retreat_fleets.size() > sim::galactic::galactic_max_objects
        || (!a.land && (!a.land_invaders.empty() || a.fix_land_structures))) return false;
    std::set<sim::EntityId> ids;
    for (const auto& death : a.deaths) if (!death.id || !death.owner || !ids.insert(death.id).second) return false;
    sim::EntityId last{};
    for (const auto& force : a.result.forces) {
        if (!force.id || force.id <= last || force.disposition > ForceDisposition::destroy_after_tactical) return false;
        last = force.id;
    }
    ids.clear();
    for (const auto id : a.land_invaders) if (!id || !ids.insert(id).second
        || std::binary_search(a.destroyed.begin(), a.destroyed.end(), id)) return false;
    ids.clear();
    for (const auto id : a.retreat_fleets) if (!id || !ids.insert(id).second) return false;
    for (const auto& row : a.record) if (row.original_type.empty() || !row.owner) return false;
    return true;
}
}

core::Result<AutoResolveOutcomeApplication> plan_auto_resolve_outcome(const ConflictRecord& conflict,
    const AutoResolveRoster& roster, const AutoResolveRoundOutcome& outcome, const AutoResolveOutcomeInputs& inputs) {
    if (roster.planet.id != conflict.planet || (outcome.winner != conflict.invader && outcome.winner != conflict.defender))
        return fail<AutoResolveOutcomeApplication>("outcome does not match its conflict");
    AutoResolveOutcomeApplication a; a.planet = conflict.planet; a.invader = conflict.invader; a.defender = conflict.defender;
    a.land = conflict.layer == BattleLayer::land; a.fix_land_structures = outcome.fix_land_structures;
    a.planet_type = inputs.planet_type; a.learning = outcome.learning;
    a.result.conflict = conflict.id; a.result.winner = outcome.winner; a.result.station_level = conflict.station_level;
    if (outcome.retreating_player) a.result.retreat_player = outcome.retreating_player;
    a.pending_superweapon_victory = outcome.superweapon_victory;
    std::map<sim::EntityId, const AutoResolveObject*> objects;
    std::map<sim::EntityId, const AutoResolveFleet*> fleets;
    for (const auto& row : roster.objects) if (!row.id || !objects.emplace(row.id, &row).second)
        return fail<AutoResolveOutcomeApplication>("duplicate outcome object identity");
    for (const auto& row : roster.fleets) if (!row.fleet.id || objects.contains(row.fleet.id) || !fleets.emplace(row.fleet.id, &row).second)
        return fail<AutoResolveOutcomeApplication>("duplicate outcome fleet identity");
    std::set<sim::EntityId> dead, allocated, delivered;
    std::map<sim::EntityId, sim::galactic::PlayerId> final_blows;
    const auto record_object = [&](const sim::EntityId id) -> bool {
        const auto found = objects.find(id);
        if (found == objects.end()) return false;
        a.record.push_back({found->second->original_type, found->second->owner}); return true;
    };
    const auto add_death = [&](const AutoResolveAllocation& loss) -> core::Result<void> {
        if (!delivered.insert(loss.id).second) return core::Result<void>::success();
        a.deaths.push_back({loss.id, loss.killer, loss.owner, loss.final_blow_player});
        const auto object = objects.find(loss.id);
        const bool history_before = !a.land && object != objects.end()
            && object->second->kind == AutoResolveObjectKind::transport;
        if (history_before) {
            for (const auto id : object->second->contents) if (!record_object(id))
                return fail<void>("transport battle record needs its original contained roster");
        }
        if (!objects.contains(loss.id) && !fleets.contains(loss.id)) return fail<void>("unknown ordinary casualty identity");
        if (!inputs.lethal_closure) return fail<void>("damage request requires explicit WAR-U04 post-delivery casualties");
        auto closure = inputs.lethal_closure(loss);
        if (!closure) return core::Result<void>::failure(closure.error());
        for (const auto id : closure.value()) {
            if (!id || (!objects.contains(id) && !fleets.contains(id))) return fail<void>("invalid WAR-U04 lethal closure identity");
            dead.insert(id);
            final_blows[id] = loss.final_blow_player;
        }
        if (history_before) return core::Result<void>::success();
        if (const auto found = fleets.find(loss.id); found != fleets.end()) {
            if (!inputs.post_delivery_record) return fail<void>("fleet attrition history requires its post-delivery contents");
            auto history = inputs.post_delivery_record(loss);
            if (!history) return core::Result<void>::failure(history.error());
            a.record.insert(a.record.end(), history.value().begin(), history.value().end());
        } else if (const auto object_it = objects.find(loss.id); object_it != objects.end()) {
            if (object_it->second->kind == AutoResolveObjectKind::transport) {
                if (!inputs.post_delivery_record) return fail<void>("company attrition history requires its post-delivery contents");
                auto history = inputs.post_delivery_record(loss);
                if (!history) return core::Result<void>::failure(history.error());
                a.record.insert(a.record.end(), history.value().begin(), history.value().end());
            } else if (!record_object(loss.id)) return fail<void>("missing casualty record object");
        } else return fail<void>("unknown ordinary casualty identity");
        return core::Result<void>::success();
    };
    for (const auto& allocation : outcome.allocations) {
        if (!allocation.id || !allocated.insert(allocation.id).second) return fail<AutoResolveOutcomeApplication>("duplicate allocation identity");
        if (allocation.id != conflict.planet) {
            const auto object = objects.find(allocation.id); const auto fleet = fleets.find(allocation.id);
            const auto owner = object != objects.end() ? object->second->owner : fleet != fleets.end() ? fleet->second->fleet.owner : 0;
            if (!owner || owner != allocation.owner || (owner != conflict.invader && owner != conflict.defender))
                return fail<AutoResolveOutcomeApplication>("allocation owner does not match the live roster");
        }
        if (allocation.id == conflict.planet) {
            if (allocation.base_changed || allocation.base_destroyed) {
                a.base_changed = true;
                if (!inputs.base_level) return fail<AutoResolveOutcomeApplication>("base change requires validated level lookup");
                auto level = inputs.base_level(allocation.retained_base_type);
                if (!level) return core::Result<AutoResolveOutcomeApplication>::failure(level.error());
                a.result.station_level = level.value();
                if (allocation.base_destroyed && !roster.planet.base_type.empty()) a.record.push_back({roster.planet.base_type, roster.planet.owner});
            }
        } else if (allocation.killed) {
            auto killed = add_death(allocation); if (!killed) return core::Result<AutoResolveOutcomeApplication>::failure(killed.error());
        }
    }
    if (a.land && outcome.winner == conflict.invader) {
        a.result.station_level = 0; a.fix_land_structures = true; a.base_changed = true;
        // WAR-56: ordinary lethal requests from the end of the landed list.
        for (auto it = roster.planet.landed.rbegin(); it != roster.planet.landed.rend(); ++it) {
            const auto found = objects.find(*it);
            if (found == objects.end()) return fail<AutoResolveOutcomeApplication>("landed outcome needs its object roster");
            // WGM-186: the reported surface retreat leaves before this residual scan.
            if (found->second->owner != conflict.invader && found->second->owner != outcome.retreating_player) {
                AutoResolveAllocation loss; loss.id = *it; loss.owner = found->second->owner;
                loss.final_blow_player = conflict.invader; loss.killed = true;
                const auto live = std::find_if(conflict.forces.begin(), conflict.forces.end(), [&](const auto& row) { return row.owner == conflict.invader; });
                if (live != conflict.forces.end()) loss.killer = live->id;
                auto killed = add_death(loss); if (!killed) return core::Result<AutoResolveOutcomeApplication>::failure(killed.error());
            }
        }
        const auto transport = [&](const sim::EntityId id) {
            const auto row = objects.find(id);
            return row != objects.end() && row->second->owner == conflict.invader
                && row->second->kind == AutoResolveObjectKind::transport && !dead.contains(id);
        };
        for (const auto id : roster.invader_entries) {
            if (!transport(id)) continue;
            a.land_invaders.push_back(id);
            if (std::find(inputs.raid_destination_entries.begin(), inputs.raid_destination_entries.end(), id)
                == inputs.raid_destination_entries.end()) break;
        }
        if (a.land_invaders.empty()) {
            for (const auto fleet_id : roster.orbit_fleets) {
                const auto fleet = fleets.find(fleet_id); if (fleet == fleets.end() || fleet->second->fleet.owner != conflict.invader) continue;
                for (const auto& member : fleet->second->fleet.members) if (transport(member.id)) { a.land_invaders.push_back(member.id); break; }
                if (!a.land_invaders.empty()) break;
            }
        }
    }
    // WAR-57's losing raid returns from orbit even without a solver retreat flag.
    const bool losing_raid = a.land && outcome.winner != conflict.invader
        && (conflict.raid || !inputs.raid_destination_entries.empty());
    if (losing_raid)
        a.result.retreat_player = conflict.invader;
    // WGM-201: ordinary land losses gather surface survivors only. WAR-57's
    // losing raid is the separate orbital-retreat exception.
    const bool retained_raid = losing_raid && std::any_of(roster.fleets.begin(), roster.fleets.end(),
        [&](const auto& row) { return row.fleet.owner == conflict.invader && row.fleet.raid_destination == conflict.planet; });
    if (a.result.retreat_player && (!a.land || losing_raid)) {
        for (auto it = roster.orbit_fleets.rbegin(); it != roster.orbit_fleets.rend(); ++it) {
            const auto row = fleets.find(*it);
            if (row != fleets.end() && row->second->fleet.owner == *a.result.retreat_player && !dead.contains(*it)
                && (!retained_raid || row->second->fleet.raid_destination == conflict.planet)
                && std::any_of(row->second->fleet.members.begin(), row->second->fleet.members.end(), [&](const auto& member) { return !dead.contains(member.id); }))
                a.retreat_fleets.push_back(*it);
        }
    }
    a.destroyed.assign(dead.begin(), dead.end());
    if (conflict.superweapon && dead.contains(conflict.superweapon)) {
        a.result.superweapon_destroyed = true; a.result.superweapon_killer = final_blows.at(conflict.superweapon);
    }
    for (const auto& force : conflict.forces) {
        ForceResult row; row.id = force.id;
        if (dead.contains(force.id)) { row.disposition = ForceDisposition::destroyed; row.hero_defeated = force.hero;
            row.final_blow_player = final_blows.at(force.id);
        } else if (a.result.retreat_player && force.owner == *a.result.retreat_player) row.disposition = ForceDisposition::retreated;
        a.result.forces.push_back(row);
    }
    if (!valid(a)) return fail<AutoResolveOutcomeApplication>("invalid outcome application");
    return core::Result<AutoResolveOutcomeApplication>::success(std::move(a));
}

core::Result<void> apply_auto_resolve_outcome(sim::galactic::GalacticSession& galaxy,
    const AutoResolveOutcomeApplication& a, const AutoResolveOutcomeServices& services) {
    if (!valid(a)) return fail<void>("invalid auto-resolve application");
    if ((a.land || a.base_changed) && !services.control)
        return fail<void>("ControlOutcomeApplication requires the political-control return adapter");
    if (a.result.retreat_player && !services.retreat)
        return fail<void>("RetreatOutcomeApplication requires the WGM orbital/surface return adapter");
    if (services.war_u04_lethal_effects) {
        auto applied = services.war_u04_lethal_effects(galaxy, a.deaths); if (!applied) return applied;
    }
    // WAR-60/87: the kill path pays bounties during delivery, before any ownership return.
    if (services.bounty) { auto paid = services.bounty(galaxy, a); if (!paid) return paid; }
    // WGM-186 / WAR-49/56: surface survivors leave before winning cleanup.
    // Solver damage delivery remains before this return-side retreat.
    if (a.land && a.result.retreat_player && services.retreat) { auto applied = services.retreat(galaxy, a); if (!applied) return applied; }
    // Land/control validates living source identities before fleet removal.
    if (services.control) { auto applied = services.control(galaxy, a); if (!applied) return applied; }
    {
        // WGP-98: each confirmed loss is credited to its recorded final blow.
        std::map<sim::EntityId, sim::galactic::PlayerId> blows;
        for (const auto& row : a.result.forces)
            if (row.disposition == ForceDisposition::destroyed && row.final_blow_player) blows.emplace(row.id, row.final_blow_player);
        for (const auto& row : a.deaths) if (row.final_blow_player) blows.emplace(row.id, row.final_blow_player);
        std::vector<sim::galactic::BattleKiller> killers;
        for (const auto& [id, killer] : blows) killers.push_back({id, killer});
        if (auto removed = galaxy.apply_battle_losses(a.destroyed, killers); !removed) return removed;
    }
    if (a.pending_superweapon_victory) {
        // WAR-47: existing outcome journal already carries this trusted result producer.
        const auto loser = a.result.winner == a.invader ? a.defender : a.invader;
        if (auto applied = galaxy.apply_superweapon_victory_return(loser, a.result.winner, loser, true); !applied) return applied;
    }
    if (a.fix_land_structures) {
        if (auto fixed = galaxy.fix_tactical_built_structures(a.planet, a.result.winner); !fixed) return fixed;
    }
    if (services.effects) { auto applied = services.effects(galaxy, a); if (!applied) return applied; }
    if ((!a.land || !a.result.retreat_player) && services.retreat) { auto applied = services.retreat(galaxy, a); if (!applied) return applied; }
    galaxy.append_outcome_events(a.planet, a.result.conflict, a.result.winner,
        a.result.winner == a.invader ? a.defender : a.invader);
    return core::Result<void>::success();
}

core::Result<std::vector<std::uint8_t>> encode_auto_resolve_outcome(const AutoResolveOutcomeApplication& a) {
    if (!valid(a)) return fail<std::vector<std::uint8_t>>("invalid outcome encoding input");
    std::vector<std::uint8_t> b;
    const auto put = [&](std::uint64_t value, const std::size_t width) { for (std::size_t i = 0; i < width; ++i) { b.push_back(static_cast<std::uint8_t>(value & 255U)); value >>= 8U; } };
    const auto text = [&](const std::string& value) { put(value.size(), 4); b.insert(b.end(), value.begin(), value.end()); };
    const auto ids = [&](const auto& rows) { put(rows.size(), 4); for (const auto id : rows) put(id, 8); };
    put(a.planet, 8); put(a.invader, 4); put(a.defender, 4); put(a.land, 1); put(a.fix_land_structures, 1); put(a.base_changed, 1); put(a.pending_superweapon_victory, 1);
    put(a.result.conflict, 8); put(a.result.winner, 4); put(a.result.retreat_player.value_or(0), 4);
    put(a.result.station_level, 4); put(a.result.superweapon_destroyed, 1); put(a.result.superweapon_killer, 4);
    put(a.result.tactical_frames, 8); put(a.result.forces.size(), 4);
    for (const auto& r : a.result.forces) { put(r.id, 8); put(static_cast<std::uint8_t>(r.disposition), 1); put(r.final_blow_player, 4); put(r.hero_defeated, 1); }
    ids(a.destroyed); put(a.deaths.size(), 4);
    for (const auto& r : a.deaths) { put(r.id, 8); put(r.killer, 8); put(r.owner, 4); put(r.final_blow_player, 4); }
    ids(a.land_invaders); ids(a.retreat_fleets); text(a.planet_type); put(a.record.size(), 4);
    for (const auto& r : a.record) { text(r.original_type); put(r.owner, 4); }
    put(a.learning.size(), 4);
    for (const auto& r : a.learning) { put(r.player, 4); put(r.id, 8); text(r.type); put(r.survived, 1); }
    if (b.size() > 1'048'576) return fail<std::vector<std::uint8_t>>("outcome payload exceeds boundary limit");
    return core::Result<std::vector<std::uint8_t>>::success(std::move(b));
}
core::Result<AutoResolveOutcomeApplication> decode_auto_resolve_outcome(const std::span<const std::uint8_t> b) {
    if (b.size() > 1'048'576) return fail<AutoResolveOutcomeApplication>("outcome payload exceeds boundary limit");
    std::size_t at{}; bool failed{};
    const auto get = [&](const std::size_t width) { std::uint64_t value{}; if (width > b.size() - at) { failed = true; return value; }
        for (std::size_t i = 0; i < width; ++i) { value |= std::uint64_t{b[at++]} << (8U * i); }
        return value; };
    const auto flag = [&]() { const auto value = get(1); if (value > 1) failed = true; return value != 0; };
    const auto count = [&](const std::size_t width) { const auto n = get(4); if (n > sim::galactic::galactic_max_objects || n > (b.size() - at) / width) { failed = true; return std::size_t{}; } return static_cast<std::size_t>(n); };
    const auto text = [&]() { const auto n = count(1); std::string value(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n)); at += n; return value; };
    const auto ids = [&](auto& rows) { const auto n = count(8); for (std::size_t i = 0; i < n; ++i) rows.push_back(get(8)); };
    AutoResolveOutcomeApplication a;
    a.planet = get(8); a.invader = static_cast<std::uint32_t>(get(4)); a.defender = static_cast<std::uint32_t>(get(4)); a.land = flag(); a.fix_land_structures = flag(); a.base_changed = flag(); a.pending_superweapon_victory = flag();
    a.result.conflict = get(8); a.result.winner = static_cast<std::uint32_t>(get(4)); const auto retreat = static_cast<std::uint32_t>(get(4)); if (retreat) a.result.retreat_player = retreat;
    a.result.station_level = static_cast<std::uint32_t>(get(4)); a.result.superweapon_destroyed = flag(); a.result.superweapon_killer = static_cast<std::uint32_t>(get(4)); a.result.tactical_frames = get(8);
    const auto forces = count(14);
    for (std::size_t i = 0; i < forces; ++i) { ForceResult r; r.id = get(8); r.disposition = static_cast<ForceDisposition>(get(1)); r.final_blow_player = static_cast<std::uint32_t>(get(4)); r.hero_defeated = flag(); a.result.forces.push_back(r); }
    ids(a.destroyed); const auto deaths = count(24);
    for (std::size_t i = 0; i < deaths; ++i) { AutoResolveDeath r; r.id = get(8); r.killer = get(8); r.owner = static_cast<std::uint32_t>(get(4)); r.final_blow_player = static_cast<std::uint32_t>(get(4)); a.deaths.push_back(r); }
    ids(a.land_invaders); ids(a.retreat_fleets); a.planet_type = text(); const auto records = count(8);
    for (std::size_t i = 0; i < records; ++i) { AutoResolveRecordEntry r; r.original_type = text(); r.owner = static_cast<std::uint32_t>(get(4)); a.record.push_back(std::move(r)); }
    const auto learning = count(17);
    for (std::size_t i = 0; i < learning; ++i) { AutoResolveLearning r; r.player = static_cast<std::uint32_t>(get(4)); r.id = get(8); r.type = text(); r.survived = flag(); a.learning.push_back(std::move(r)); }
    if (failed || at != b.size() || !valid(a)) return fail<AutoResolveOutcomeApplication>("malformed outcome payload");
    return core::Result<AutoResolveOutcomeApplication>::success(std::move(a));
}
} // namespace eawr::campaign
