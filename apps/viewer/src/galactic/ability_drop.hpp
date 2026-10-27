#pragma once
#include "galaxy_view.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/fleets.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {
struct AbilityDrop {
    ObjectId actor{}, planet{};
    sim::galactic::AbilityKind kind{};
    ObjectId supplied{};
    friend bool operator==(const AbilityDrop&, const AbilityDrop&) = default;
};
// #2042 owns the raw story/tutorial producers. Callers with those facts can
// supply them without substituting ownership or execution refusal for a gate.
struct AbilityPreviewPolicy {
    std::function<std::int64_t(sim::galactic::TypeId)> rank;
    std::function<bool(ObjectId)> restricted;
    std::function<bool(ObjectId, ObjectId)> space_override;
    std::function<bool(ObjectId, sim::galactic::AbilityKind)> kind_restricted;
};
namespace ability_preview {
template<class Rows, class Id, class Key> const auto* find(const Rows& rows, Id id, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id,
        [&](const auto& row, auto value) { return key(row) < value; });
    return at != rows.end() && key(*at) == id ? &*at : nullptr;
}
inline bool routed(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}
inline bool enabled(const sim::galactic::GalacticAbilityProfile& p,
    const sim::galactic::AbilityActorState& s, const sim::galactic::GalacticAbilitySetup& host,
    sim::galactic::AbilityKind kind) {
    using sim::galactic::AbilityKind;
    if (p.lifecycle_only || s.type != p.type) return false;
    switch (kind) {
    case AbilityKind::planet_destruction: return p.destruction && s.destruction_enabled && !s.destruction_cancelled
        && routed(host.activated_destruction_abilities, p.destruction->name);
    case AbilityKind::technology_theft: return p.theft && s.theft_enabled && !s.theft_cancelled;
    case AbilityKind::system_spy: return p.spy && s.spy_enabled && !s.spy_cancelled
        && routed(host.activated_spy_abilities, p.spy->name);
    case AbilityKind::black_market: return p.has_market && s.enabled && !s.cancelled;
    case AbilityKind::structure_sabotage: return p.sabotage && s.sabotage_enabled && !s.sabotage_cancelled
        && routed(host.activated_sabotage_abilities, p.sabotage->name);
    case AbilityKind::hero_neutralization: return p.neutralize && s.neutralize_enabled && !s.neutralize_cancelled
        && routed(host.activated_neutralize_abilities, p.neutralize->name);
    case AbilityKind::corruption: return p.corruption && s.corruption_enabled && !s.corruption_cancelled
        && routed(host.activated_corruption_abilities, p.corruption->name);
    case AbilityKind::superweapon_hack: return p.hack && s.hack_enabled && !s.hack_cancelled
        && routed(host.activated_hack_abilities, p.hack->name);
    }
    return false;
}
inline bool hack_weapon(const sim::galactic::GalacticSnapshot& snapshot, ObjectId target,
    sim::galactic::TypeId required) {
    if (!required) return false;
    for (const auto& fleet : snapshot.fleets()) if (fleet.orbit == target && !fleet.motion.transit)
        for (const auto& member : fleet.members) {
            const auto* actor = find(snapshot.actor_records(), member.id, [](const auto& row) { return row.id; });
            if (actor && !actor->dead && !actor->delete_pending && actor->current_type == required) return true;
        }
    return false;
}
// WGCH-20/48/53..55/109/113/138: target appropriateness is separate from
// affordability, contested orbit, prepared prices and leader-victory execution.
inline bool target_admitted(const sim::galactic::GalacticSnapshot& snapshot,
    const sim::galactic::CensusObject& actor, ObjectId target,
    const sim::galactic::GalacticAbilitySetup& host, sim::galactic::AbilityKind kind) {
    using namespace sim::galactic;
    const auto* planet = find(snapshot.planets(), target, [](const auto& row) { return row.id; });
    if (!planet) return false;
    if (kind == AbilityKind::superweapon_hack) return hack_weapon(snapshot, target, host.hack_required_type);
    if (kind == AbilityKind::hero_neutralization) return !hack_weapon(snapshot, target, host.hack_required_type);
    const auto* control = snapshot.control();
    if (!control) return false;
    const auto* surface = find(control->planets, target, [](const auto& row) { return row.planet; });
    if (!surface || (surface->destroyed && kind != AbilityKind::system_spy)) return false;
    const auto index = static_cast<std::size_t>(surface - control->planets.data());
    if (index >= control->state.size()) return false;
    const auto& allegiance = control->state[index];
    const auto allied = [&](PlayerId other) {
        return other && (actor.owner == other || std::any_of(control->relations.begin(), control->relations.end(),
            [&](const auto& row) { return row.player == actor.owner && row.other == other && row.allied; }));
    };
    if (kind == AbilityKind::planet_destruction)
        return (!actor.planet || actor.planet == target) && !allied(allegiance.target_player);
    if (kind == AbilityKind::system_spy) return !allied(allegiance.target_player);
    const auto* economy = snapshot.economy_corruption();
    const auto* economic = economy ? find(economy->planets, target, [](const auto& row) { return row.planet; }) : nullptr;
    if (!economic || economic->destroyed) return false;
    if (kind == AbilityKind::corruption) {
        const auto* owner = find(control->players, planet->owner, [](const auto& row) { return row.player; });
        const auto* content = find(host.corruption_planets, target, [](const auto& row) { return row.planet; });
        const auto* transition = find(snapshot.corruption_transitions(), target, [](const auto& row) { return row.planet; });
        return host.tutorial_corruption_allowed && owner && !owner->neutral && !allied(planet->owner)
            && content && content->retains_influence && economic->corruption == corruption_fixture::Type::none
            && (!transition || !transition->transition.active())
            && std::any_of(content->slots.begin(), content->slots.end(), [](const auto& row) {
                return row.type != corruption_fixture::Type::none; });
    }
    if (allied(allegiance.target_player)) return false;
    if (kind == AbilityKind::black_market || kind == AbilityKind::technology_theft) {
        const auto* current = find(economy->players, allegiance.current_player, [](const auto& row) { return row.player; });
        if (!current || !current->playable) return false;
        if (kind == AbilityKind::black_market) return economic->corruption == corruption_fixture::Type::black_market;
        if (!find(host.theft_planets, target, [](const auto& row) { return row.planet; })) return false;
        if (!snapshot.production() || !snapshot.player_economy()) return false;
        const auto& production = *snapshot.production();
        const auto* player = find(production.players, actor.owner, [](const auto& row) { return row.player; });
        const auto* account = find(snapshot.player_economy()->players, actor.owner, [](const auto& row) { return row.player; });
        if (!player || !account) return false;
        for (const auto& technology : host.technologies) {
            const auto* type = find(production.technology_types, technology.type, [](const auto& row) { return row.type; });
            if (type && type->initially_locked && type->slicer_unlockable && type->tech <= account->current_tech
                && std::binary_search(type->affiliated_players.begin(), type->affiliated_players.end(), actor.owner)
                && !std::binary_search(player->locked.begin(), player->locked.end(), type->type)
                && !std::binary_search(player->unlocked.begin(), player->unlocked.end(), type->type)) return true;
        }
        return false;
    }
    if (kind == AbilityKind::structure_sabotage && economic->corruption != corruption_fixture::Type::none && snapshot.production()) {
        const auto* production = find(snapshot.production()->planets, target, [](const auto& row) { return row.planet; });
        if (production) for (const auto& object : production->objects) {
            const auto* cost = find(host.sabotage_costs, object.type, [](const auto& row) { return row.type; });
            if (object.kind == ProductionKind::structure && cost && cost->cost >= corruption_fixture::one) return true;
        }
    }
    return false;
}

}
// WGM-83/101: an ordinary move by an ability-dependent fleet retains the same
// supplied live container as a special-slot move. Arrival dispatch owns activation.
[[nodiscard]] inline std::optional<sim::galactic::MoveFleet> ability_move_order(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local,
    sim::galactic::MoveFleet order, const std::optional<AbilityDrop>& destination,
    std::span<const ObjectId> stack = {}) {
    if (order.path.size() < 2 || !snapshot.fleet_control()) return std::nullopt;
    const auto fleets = snapshot.fleets();
    const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == order.fleet; });
    if (fleet == fleets.end() || fleet->owner != local) return std::nullopt;
    auto merged = *fleet;
    // A stacked move submits merges first; inspect the members that will actually travel.
    for (const auto id : stack) if (id && id != fleet->id) {
        const auto other = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == id; });
        if (other == fleets.end() || other->owner != local || other->orbit != fleet->orbit || other->motion.transit)
            return std::nullopt;
        merged.members.insert(merged.members.end(), other->members.begin(), other->members.end());
    }
    const auto ordinary = sim::galactic::fleet_move_without_ability(
        merged, snapshot.fleet_control()->types, snapshot.movement());
    if (!ordinary) return std::nullopt;
    if (ordinary.value() || order.raid || order.destination_actor) return order;
    if (!destination || !destination->supplied || destination->planet != order.path.back()) return std::nullopt;
    order.destination_actor = destination->supplied;
    return order;
}
// WGCH-152/153: rank the supplied breakdown BEFORE trying abilities. First wins
// ties, including ranks above 999; an ineligible top actor never falls through.
[[nodiscard]] inline std::optional<AbilityDrop> ability_drop_candidate(
    const sim::galactic::GalacticSnapshot& snapshot, PlayerId local, ObjectId fleet_id,
    const sim::galactic::GalacticAbilitySetup& host, const AbilityPreviewPolicy& policy = {}, ObjectId destination = 0) {
    using namespace sim::galactic;
    using ability_preview::find;
    const auto* fleet = find(snapshot.fleets(), fleet_id, [](const auto& row) { return row.id; });
    sim::galactic::Fleet surface;
    if (!fleet && snapshot.control()) {
        for (const auto& planet : snapshot.control()->planets)
            for (const auto& force : planet.landed) if (force.id == fleet_id && force.owner == local) {
                surface.id = fleet_id; surface.owner = local; surface.orbit = planet.planet;
                surface.members = {{force.id, force.type}}; fleet = &surface;
            }
    }
    if (!fleet || fleet->owner != local || fleet->motion.transit || fleet->members.empty()) return std::nullopt;
    const auto target = destination ? destination : fleet->orbit;
    const auto source_planet = fleet->orbit;
    if (policy.restricted && policy.restricted(target)) return std::nullopt;
    const CensusObject* selected = nullptr;
    std::int64_t best{};
    for (const auto& member : fleet->members) {
        const auto* actor = find(snapshot.actor_records(), member.id, [](const auto& row) { return row.id; });
        if (!actor || actor->dead || actor->delete_pending || actor->owner != local
            || actor->container != (fleet == &surface ? source_planet : fleet_id) || actor->planet != source_planet) return std::nullopt;
        const auto rank = policy.rank ? policy.rank(actor->original_type) : 25;
        if (!selected || rank < best) { selected = actor; best = rank; }
    }
    if (!selected || (policy.space_override && policy.space_override(selected->id, target))) return std::nullopt;
    // Missing siphon/removal runtimes remain #1143/#1141; never disguise them as another kind.
    constexpr std::array kinds{AbilityKind::planet_destruction, AbilityKind::technology_theft, AbilityKind::black_market,
        AbilityKind::structure_sabotage, AbilityKind::system_spy, AbilityKind::hero_neutralization, AbilityKind::corruption, AbilityKind::superweapon_hack};
    for (const auto kind : kinds) {
        if (policy.kind_restricted && policy.kind_restricted(target, kind)) continue;
        std::vector<ObjectId> pending{selected->id};
        std::set<ObjectId> visited;
        while (!pending.empty()) {
            const auto id = pending.back(); pending.pop_back();
            if (visited.size() >= snapshot.actor_records().size()
                || !visited.insert(id).second) return std::nullopt;
            const auto* actor = find(snapshot.actor_records(), id, [](const auto& row) { return row.id; });
            if (!actor || actor->dead || actor->delete_pending || actor->owner != local || actor->planet != source_planet) continue;
            const auto* movement = snapshot.movement();
            const auto* traits = movement ? find(movement->types, actor->current_type, [](const auto& row) { return row.type; }) : nullptr;
            if (traits && (traits->transport || traits->fleet_container)) {
                const auto* contents = find(movement->contents, id, [](const auto& row) { return row.container; });
                if (!contents || (traits->fleet_container && contents->children.size() > 1)) continue;
                for (auto child = contents->children.rbegin(); child != contents->children.rend(); ++child) {
                    const auto* live = find(snapshot.actor_records(), child->id, [](const auto& row) { return row.id; });
                    if (live && live->container == id) pending.push_back(child->id);
                }
                continue;
            }
            const auto* profile = find(host.profiles, actor->current_type, [](const auto& row) { return row.type; });
            const auto* service = find(snapshot.ability_services(), actor->id, [](const auto& row) { return row.actor; });
            if (!profile || !service || !ability_preview::enabled(*profile, *service, host, kind)
                || (target != source_planet && !ground_ability_enabled(*profile, *service, kind))
                || !ability_preview::target_admitted(snapshot, *actor, target, host, kind)) continue;
            if (fleet->members.size() > 1 && kind != AbilityKind::planet_destruction) return std::nullopt;
            return AbilityDrop{kind == AbilityKind::planet_destruction ? selected->id : actor->id, target, kind, selected->id};
        }
    }
    return std::nullopt;
}
// Cache one eligible action per destination when the dragged fleet or snapshot changes.
template<class Planets, class Resolve>
[[nodiscard]] std::vector<AbilityDrop> ability_drop_candidates(const Planets& planets, Resolve resolve) {
    std::vector<AbilityDrop> result;
    for (const auto& planet : planets)
        if (auto drop = resolve(planet.id)) result.push_back(*drop);
    return result;
}

}
