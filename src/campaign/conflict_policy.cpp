#include "eawr/campaign/conflict_policy.hpp"

#include <algorithm>
#include <cassert>
#include <map>
#include <set>

namespace eawr::campaign {
namespace {
using sim::math::Fixed;
template<class T> core::Result<T> invalid(const char* message) {
    core::Diagnostic d; d.code = std::string(sim::galactic::diagnostic_codes::invalid_setup); d.message = message;
    return core::Result<T>::failure(std::move(d));
}
}
bool triggers_space_combat(const CombatantFacts& object) {
    // WGM-104: ordinary trigger is !stealth, independent of AI combat power.
    if (object.kind == CombatantKind::company || object.kind == CombatantKind::squadron) return true;
    if (object.kind == CombatantKind::space_base) return true;
    if (object.kind == CombatantKind::ground_base) return false;
    return object.combatant && !object.stealth;
}
bool can_participate_space_combat(const CombatantFacts& object) {
    if (object.deleted) return false;
    if (object.kind == CombatantKind::fleet || object.kind == CombatantKind::transport)
        return std::any_of(object.contents.begin(), object.contents.end(), can_participate_space_combat);
    return true; // WGM-105: generic object predicate does not require a combatant interface.
}
bool can_participate_land_combat(const CombatantFacts& object) {
    if (object.kind == CombatantKind::ground_base) return true;
    if (object.kind == CombatantKind::space_base || object.kind == CombatantKind::squadron) return false;
    if (object.kind == CombatantKind::company)
        return object.associated_object && !object.contents.empty() && can_participate_land_combat(object.contents.front());
    if (object.kind != CombatantKind::transport || !object.associated_object || object.stays_in_transport) return false;
    return std::any_of(object.contents.begin(), object.contents.end(), [](const auto& payload) {
        return payload.combatant && payload.ai_power.raw() > 0 && !payload.stays_in_transport;
    }); // WGM-106/107: positive payload power, and both transport/payload stay flags.
}
core::Result<LayerPresence> collect_conflict_presence(const sim::galactic::GalacticSnapshot& snapshot,
    const sim::galactic::GalacticEvent& request, const ForceQualifier& qualify, const FleetQualifier& qualify_fleet,
    const sim::galactic::NativeInvasion* explicit_invasion) {
    namespace gc = sim::galactic;
    if (!qualify || !qualify_fleet || (request.kind != gc::GalacticEventKind::movement_conflict
        && request.kind != gc::GalacticEventKind::land_conflict_requested))
        return invalid<LayerPresence>("invalid conflict request or missing live qualifier");
    const auto fleet = std::find_if(snapshot.fleets().begin(), snapshot.fleets().end(), [&](const auto& row) { return row.id == request.subject; });
    const auto planet = std::find_if(snapshot.planets().begin(), snapshot.planets().end(), [&](const auto& row) { return row.id == request.value; });
    if (planet == snapshot.planets().end() || (!explicit_invasion && (fleet == snapshot.fleets().end() || fleet->orbit != planet->id)))
        return invalid<LayerPresence>("conflict request has no live arriving fleet at its planet");
    LayerPresence out; out.invader = explicit_invasion ? explicit_invasion->invader : fleet->owner;
    out.defender = explicit_invasion ? explicit_invasion->defender : planet->owner;
    const gc::ControlPlanet* surface = nullptr;
    if (const auto* control = snapshot.control()) {
        const auto found = std::find_if(control->planets.begin(), control->planets.end(), [&](const auto& row) { return row.planet == planet->id; });
        if (found != control->planets.end()) surface = &*found;
    }
    if (surface) {
        out.starbase_owner = surface->starbase.owner; out.starbase_level = surface->starbase.level;
        out.ground_base_level = surface->ground_base.level;
        if (!explicit_invasion && request.kind == gc::GalacticEventKind::land_conflict_requested) {
            const auto* control = snapshot.control();
            const auto index = static_cast<std::size_t>(surface - control->planets.data());
            assert(index < control->state.size() && control->state[index].planet == planet->id);
            // WGS-186: a valid nonneutral target precedes the first linked owner.
            out.defender = surface->ground_base.level ? surface->ground_base.owner : control->state[index].target_player;
            const auto owner = std::find_if(control->players.begin(), control->players.end(),
                [&](const auto& row) { return row.player == out.defender; });
            if (!surface->ground_base.level && (!out.defender || (owner != control->players.end() && owner->neutral)) && !surface->landed.empty())
                out.defender = surface->landed.front().owner;
        }
    }
    const auto append = [&](std::span<const gc::FleetMember> members, gc::PlayerId owner, sim::EntityId parent,
        std::vector<AdmissionForce>& destination) -> core::Result<void> {
        std::vector<gc::FleetMember> pending(members.rbegin(), members.rend());
        std::set<sim::EntityId> seen;
        while (!pending.empty()) {
            const auto member = pending.back(); pending.pop_back();
            if (!seen.insert(member.id).second || seen.size() > gc::galactic_max_objects)
                return invalid<void>("cyclic or oversized conflict breakdown");
            auto row = qualify(member, owner, parent);
            if (!row) return core::Result<void>::failure(row.error());
            if (row.value().force.id != member.id || row.value().force.type != member.type
                || row.value().force.owner != owner || row.value().force.parent != parent)
                return invalid<void>("live qualifier changed persistent conflict identity");
            const auto* movement = snapshot.movement();
            const auto contents = movement ? std::find_if(movement->contents.begin(), movement->contents.end(),
                [&](const auto& c) { return c.container == member.id; }) : std::vector<gc::MovementContents>::const_iterator{};
            if (row.value().deleted) continue;
            if (movement && contents != movement->contents.end() && !row.value().transport && !row.value().whole_squadron)
                pending.insert(pending.end(), contents->children.rbegin(), contents->children.rend());
            else destination.push_back(std::move(row).value());
        }
        return core::Result<void>::success();
    };
    if (explicit_invasion) {
        for (const auto& member : explicit_invasion->transports) {
            for (const auto& parent : snapshot.fleets()) {
                const auto current = std::find_if(parent.members.begin(), parent.members.end(), [&](const auto& row) { return row.id == member.id; });
                if (parent.owner != out.invader || parent.orbit != planet->id || parent.motion.transit
                    || current == parent.members.end()) continue;
                auto row = qualify(*current, parent.owner, parent.id);
                if (!row) return core::Result<LayerPresence>::failure(row.error());
                out.invading.push_back(row.value());
                if (auto added = append(std::span(&*current, 1), parent.owner, parent.id, out.invading_forces); !added) return core::Result<LayerPresence>::failure(added.error());
                break;
            }
        }
    } else {
        for (const auto& member : fleet->members) {
            auto row = qualify(member, fleet->owner, fleet->id);
            if (!row) return core::Result<LayerPresence>::failure(row.error());
            out.invading.push_back(std::move(row).value());
        }
        if (auto added = append(fleet->members, fleet->owner, fleet->id, out.invading_forces); !added) return core::Result<LayerPresence>::failure(added.error());
    }
    std::vector<const gc::Fleet*> orbit;
    for (const auto& row : snapshot.fleets()) if (row.orbit == planet->id && (explicit_invasion || row.id != fleet->id)) orbit.push_back(&row);
    std::stable_sort(orbit.begin(), orbit.end(), [](const auto* a, const auto* b) { return a->motion.orbit_entry < b->motion.orbit_entry; });
    for (const auto* row : orbit) {
        auto qualified = qualify_fleet(*row);
        if (!qualified) return core::Result<LayerPresence>::failure(qualified.error());
        AttachedFleetPresence attached{row->owner, qualified.value(), {}};
        for (const auto& member : row->members) {
            auto direct = qualify(member, row->owner, row->id);
            if (!direct) return core::Result<LayerPresence>::failure(direct.error());
            attached.direct_members.push_back(std::move(direct).value());
        }
        out.attached_fleets.push_back(std::move(attached));
        if (auto added = append(row->members, row->owner, row->id, out.orbital_forces); !added)
            return core::Result<LayerPresence>::failure(added.error());
    }
    if (!explicit_invasion && request.kind == gc::GalacticEventKind::movement_conflict) {
        // WGM-202: the space defender is never read from the planet owner. It is the first orbit-list fleet that is not
        // stealth and not the invader's, else a nonzero starbase the invader does not own; no candidate means no battle.
        out.defender = 0;
        for (const auto& fleet_row : out.attached_fleets)
            if (fleet_row.owner != out.invader && !skips_space_combat(fleet_row.direct_members)) { out.defender = fleet_row.owner; break; }
        if (!out.defender && out.starbase_level != 0 && out.starbase_owner != out.invader) out.defender = out.starbase_owner;
    }
    if (surface) for (const auto& force : surface->landed) {
        const gc::FleetMember member{force.id, force.type};
        auto row = qualify(member, force.owner, planet->id);
        if (!row) return core::Result<LayerPresence>::failure(row.error());
        out.landed.push_back(std::move(row).value());
    }
    return core::Result<LayerPresence>::success(std::move(out));
}
bool admits_space_conflict(const LayerPresence& p) {
    if (p.prepared_resolver) return true; // WGM-103: PREPARED, not merely initiated.
    if (p.invader == p.defender) return false;
    bool attacker = false, defender = p.starbase_level != 0 && p.starbase_owner == p.defender;
    const auto inspect = [&](const auto& rows) {
        for (const auto& row : rows) if (!row.deleted && row.space_allowed && row.triggers_space) {
            attacker = attacker || row.force.owner == p.invader;
            defender = defender || row.force.owner == p.defender;
        }
    };
    inspect(p.invading);
    for (const auto& fleet : p.attached_fleets) {
        if (fleet.facts.deleted || !fleet.facts.space_allowed || !fleet.facts.fleet_behavior) continue;
        // WGM-105: direct children have no independent delete/type gate or recursion.
        for (const auto& row : fleet.direct_members) if (row.triggers_space || row.fleet_behavior) {
            attacker = attacker || row.force.owner == p.invader;
            defender = defender || row.force.owner == p.defender;
        }
    }
    return attacker && defender;
}
bool admits_land_conflict(const LayerPresence& p) {
    if (p.prepared_resolver) return true;
    if (p.invader == p.defender) return false;
    bool attacker = std::any_of(p.invading.begin(), p.invading.end(), [](const auto& row) {
        return !row.deleted && row.ground_capable;
    });
    for (const auto& fleet : p.attached_fleets) if (!fleet.facts.deleted && fleet.owner == p.invader) {
        attacker = attacker || fleet.facts.ground_capable
            || std::any_of(fleet.direct_members.begin(), fleet.direct_members.end(), [](const auto& row) { return row.ground_capable; });
    }
    if (p.ground_base_level != 0) return attacker;
    // WGM-42: the first capable surface entry determines the branch, including an invader entry.
    for (const auto& row : p.landed) if (!row.deleted && row.ground_capable) {
        if (row.force.owner == p.invader) return false;
        return attacker;
    }
    return false;
}
bool skips_space_combat(const std::span<const AdmissionForce> forces) {
    return !forces.empty() && std::all_of(forces.begin(), forces.end(), [](const auto& row) {
        return row.deleted || row.skips_combat;
    });
}
bool raid_capable(const std::span<const AdmissionForce> forces, const RaidRules& rules) {
    if (forces.empty() || (rules.required_faction && forces.front().faction != rules.required_faction)) return false;
    std::uint32_t heroes = 0, limited = 0;
    for (const auto& row : forces) {
        if (row.transport && row.stays_in_transport) return false;
        if (row.force.hero && ++heroes > rules.max_heroes) return false;
        if ((row.categories & rules.free_categories) == 0) {
            if ((row.categories & rules.limited_categories) == 0 || ++limited > rules.max_limited) return false;
        }
    }
    return limited != 0; // WGM-37: free heroes alone cannot raid.
}
core::Result<Arbitration> arbitrate_conflict(const ArbitrationInputs& p) {
    if (p.custom_option > 3 || p.invader_power.raw() < 0 || p.defender_power.raw() < 0 || p.overrun_multiple.raw() < 0)
        return invalid<Arbitration>("invalid conflict arbitration inputs");
    Arbitration out; out.forced_tactical = p.tutorial_disables_auto || p.story_restricts_auto || p.custom_option == 1;
    if (p.multiplayer) {
        if (!p.invader_human || !p.defender_human || (!out.forced_tactical && p.custom_option == 0)) {
            out.route = ConflictRoute::instant_auto; return core::Result<Arbitration>::success(out);
        }
    } else {
        if (!out.forced_tactical && p.overrun_multiple.raw() != 0) {
            const bool defender_weaker = p.defender_power <= p.invader_power;
            auto scaled = sim::math::multiply(std::min(p.invader_power, p.defender_power), p.overrun_multiple);
            if (!scaled) return core::Result<Arbitration>::failure(scaled.error());
            out.overrun = scaled.value() < std::max(p.invader_power, p.defender_power)
                && !(defender_weaker ? p.defender_named_hero : p.invader_named_hero);
        }
        // WAR-02/03: a forcing gate does not veto uninvolved/profile instant resolution.
        if (!p.local_involved || p.profile_auto || out.overrun) {
            out.route = ConflictRoute::instant_auto; return core::Result<Arbitration>::success(out);
        }
        if (!out.forced_tactical && p.custom_option == 0) {
            out.route = ConflictRoute::dialog_auto; return core::Result<Arbitration>::success(out);
        }
    }
    if (!out.forced_tactical && p.custom_option != 2) out.route = ConflictRoute::vote;
    else out.route = p.queue_tactical ? ConflictRoute::pending_tactical : ConflictRoute::tactical;
    return core::Result<Arbitration>::success(out);
}
core::Result<ForceSelection> select_conflict_forces(const std::span<const AdmissionForce> forces,
    Fixed cap, const std::optional<sim::galactic::TypeId> preferred, const bool use_pathfinder) {
    if (cap.raw() < 0 || forces.size() > sim::galactic::galactic_max_objects)
        return invalid<ForceSelection>("invalid force population cap or roster size");
    std::set<sim::EntityId> ids;
    for (const auto& row : forces) if (!row.force.id || !ids.insert(row.force.id).second || row.population.raw() < 0)
        return invalid<ForceSelection>("invalid force identity or population");
    ForceSelection out;
    const auto start = [&](const auto& row) { auto force = row.force; force.role = ForceRole::deployed; out.initial.push_back(force); };
    const auto reserve = [&](const auto& row) { auto force = row.force; force.role = ForceRole::reinforcement; out.reinforcement.push_back(force); };
    const AdmissionForce* selected = nullptr;
    if (preferred || use_pathfinder) for (const auto& row : forces) {
        if (!row.deleted && (preferred ? row.force.type == *preferred : row.pathfinder)) { selected = &row; break; }
    }
    std::map<sim::galactic::TypeId, std::vector<const AdmissionForce*>> buckets;
    std::int64_t transports = 0;
    for (const auto& row : forces) {
        if (row.deleted) continue;
        if (row.superweapon || &row == selected) { start(row); continue; }
        if (selected) { reserve(row); continue; }
        if (row.transport) {
            if (transports < cap.trunc_to_integer()) { ++transports; start(row); } else reserve(row);
        } else buckets[row.force.type].push_back(&row);
    }
    const auto take = [&](auto& bucket) { start(*bucket.back()); bucket.pop_back(); };
    const auto consume = [&](Fixed population) {
        auto next = sim::math::subtract(cap, population);
        if (!next) return false;
        cap = next.value(); return true;
    };
    for (auto& [type, bucket] : buckets) {
        (void)type;
        const auto population = bucket.front()->population;
        if (!std::all_of(bucket.begin(), bucket.end(), [&](const auto* row) { return row->population == population; }))
            return invalid<ForceSelection>("one force type has inconsistent population");
        if (population.raw() < Fixed::scale) while (!bucket.empty()) take(bucket);
        else if (population <= cap) {
            if (!consume(population)) return invalid<ForceSelection>("force population debit overflows");
            take(bucket);
        }
    }
    std::uint64_t remaining = 0; for (const auto& [type, bucket] : buckets) { (void)type; remaining += bucket.size(); }
    const auto snapshot = cap;
    // WGT-07: all pass-2 quotas use one cap snapshot; only then debit total population.
    if (remaining) for (auto& [type, bucket] : buckets) {
        (void)type; if (bucket.empty()) continue;
        auto fraction = Fixed::from_ratio(static_cast<std::int64_t>(bucket.size()), static_cast<std::int64_t>(remaining));
        if (!fraction) return core::Result<ForceSelection>::failure(fraction.error());
        auto share = sim::math::multiply(fraction.value(), snapshot);
        if (!share) return core::Result<ForceSelection>::failure(share.error());
        auto rounding = sim::math::add(share.value(), Fixed::from_raw(Fixed::scale / 2));
        if (!rounding) return core::Result<ForceSelection>::failure(rounding.error());
        const auto rounded = rounding.value().trunc_to_integer();
        const auto population = bucket.front()->population;
        auto quota = sim::math::divide(Fixed::from_integer(rounded).value(), population);
        if (!quota) return core::Result<ForceSelection>::failure(quota.error());
        const auto count = std::min<std::size_t>(bucket.size(), static_cast<std::size_t>(quota.value().trunc_to_integer()));
        for (std::size_t i = 0; i < count; ++i) {
            if (!consume(population)) return invalid<ForceSelection>("force population debit overflows");
            take(bucket);
        }
    }
    for (auto& [type, bucket] : buckets) {
        (void)type;
        while (!bucket.empty() && bucket.back()->population <= cap) {
            if (!consume(bucket.back()->population)) return invalid<ForceSelection>("force population debit overflows");
            take(bucket);
        }
        while (!bucket.empty()) { reserve(*bucket.back()); bucket.pop_back(); }
    }
    return core::Result<ForceSelection>::success(std::move(out));
}
} // namespace eawr::campaign
