#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/hero_actions.hpp"

#include <algorithm>
#include <limits>

// WGCH-27..30/96..99/135: live special-structure sabotage on the shared ability host. The prepared choice carries the
// structure identity and the displayed escalated price; execution resolves actor and structure again.
namespace eawr::sim::galactic {
namespace {
using math::Fixed;
template<class Rows, class Id, class Key> auto find(Rows& rows, Id id, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id,
        [&](const auto& row, auto value) { return key(row) < value; });
    return at != rows.end() && key(*at) == id ? &*at : nullptr;
}
template<class Rows, class Value, class Key> void put(Rows& rows, Value value, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), key(value),
        [&](const auto& row, auto id) { return key(row) < id; });
    if (at != rows.end() && key(*at) == key(value)) *at = std::move(value);
    else rows.insert(at, std::move(value));
}
constexpr std::array<ProductionDomain, 2> layers{ProductionDomain::ground, ProductionDomain::space};
}

// WGCH-77/97: a corrupted planet, the authored routing, an enabled service and a non-allied target allegiance.
bool GalacticSession::Impl::sabotage_admitted(const CensusObject& actor, const PlanetId planet) const {
    const auto* profile = find(setup.ability_host->profiles, actor.current_type, [](const auto& r) { return r.type; });
    const auto* service = find(ability_host->actors, actor.id, [](const auto& r) { return r.actor; });
    if (!profile || !profile->sabotage || !service || !service->sabotage_enabled || service->sabotage_cancelled
        || !control || !economy_corruption || !production) return false;
    const auto& names = setup.ability_host->activated_sabotage_abilities;
    if (std::find(names.begin(), names.end(), profile->sabotage->name) == names.end()) return false;
    const auto* surface = find(control->planets, planet, [](const auto& r) { return r.planet; });
    const auto* economic = find(economy_corruption->planets, planet, [](const auto& r) { return r.planet; });
    if (!surface || surface->destroyed || !economic || !planets.contains(planet)
        || economic->corruption == corruption_fixture::Type::none) return false;
    const auto index = static_cast<std::size_t>(surface - control->planets.data());
    if (index >= production->planets.size() || production->planets[index].planet != planet) return false;
    const auto target = control->state[index].target_player;
    if (target) {
        if (target == actor.owner) return false;
        for (const auto& relation : control->relations)
            if (relation.player == actor.owner && relation.other == target && relation.allied) return false;
    }
    return true;
}

void GalacticSession::Impl::prepare_sabotage(const GameCommand& command, const CensusObject& actor,
    const std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto target = std::get<PrepareGalacticAbility>(command.payload).planet;
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    if (!economy || !sabotage_admitted(actor, target)) return reject();
    if (!find(economy->players, actor.owner, [](const auto& p) { return p.player; })) return reject();
    const auto* profile = find(setup.ability_host->profiles, actor.current_type, [](const auto& r) { return r.type; });
    const auto* surface = find(control->planets, target, [](const auto& r) { return r.planet; });
    const auto& row = production->planets[static_cast<std::size_t>(surface - control->planets.data())];
    const auto* history = find(ability_host->sabotage_history, target, [](const auto& r) { return r.planet; });
    const auto prior = history ? history->count : 0U;
    AbilityPreparation prepared; prepared.actor = actor.id; prepared.planet = target;
    prepared.kind = AbilityKind::structure_sabotage; prepared.open = true;
    // WGCH-97: ground specials first, then space; prices below one are not targets; escalation is per planet history.
    for (const auto layer : layers) for (const auto& object : row.objects) {
        if (object.kind != ProductionKind::structure || object.domain != layer) continue;
        const auto* cost = find(setup.ability_host->sabotage_costs, object.type, [](const auto& r) { return r.type; });
        if (!cost || cost->cost < corruption_fixture::one) continue;
        auto price = hero_actions::sabotage_price(cost->cost, profile->sabotage->cost_factor, prior);
        if (!price) { failed = true; return; }
        prepared.target_choices.push_back({object.id, 0, price.value(), Fixed{}, Fixed{}, true}); // WGCH-77: menu absence zero
    }
    if (prepared.target_choices.empty()) return reject();
    prepared.id = ability_host->next_request++;
    if (auto* menu = find(economy_corruption->menus, actor.id, [](const auto& m) { return m.buyer; }); menu && menu->active) {
        menu->active = false; dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
    event.kind = GalacticEventKind::ability_prepared; event.value = prepared.id;
    put(ability_host->preparations, std::move(prepared), [](const auto& p) { return p.actor; });
    touch_abilities(); events.push_back(event);
}

void GalacticSession::Impl::execute_sabotage(const GameCommand& command, const CensusObject& actor,
    const PreparedAbility& prepared, const std::uint64_t tick, std::vector<GalacticEvent>& events) {
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    if (!economy || !production || !control || !ability_consumption_handler || !planets.contains(prepared.planet)) return reject();
    auto* account = find(economy->players, actor.owner, [](const auto& p) { return p.player; });
    const auto* surface = find(control->planets, prepared.planet, [](const auto& r) { return r.planet; });
    // WGCH-96: no repriced escalation, corruption or opposing-planet check; the structure must still be listed here.
    if (!account || !surface || prepared.price.raw() < 0 || account->credits < prepared.price) return reject();
    const auto index = static_cast<std::size_t>(surface - control->planets.data());
    if (index >= production->planets.size() || production->planets[index].planet != prepared.planet) return reject();
    auto& row = production->planets[index];
    const ProductionObject* found = nullptr;
    // WGCH-29: the same identity in the ground specials first, then the space specials.
    for (const auto layer : layers) {
        for (const auto& object : row.objects)
            if (object.id == prepared.target && object.kind == ProductionKind::structure && object.domain == layer) { found = &object; break; }
        if (found) break;
    }
    if (!found) return reject();
    const auto* history = find(ability_host->sabotage_history, prepared.planet, [](const auto& r) { return r.planet; });
    if (history && history->count == std::numeric_limits<std::uint32_t>::max()) return reject();
    auto defeat = prepare_ability_defeat(actor, prepared.absence, true);
    if (!defeat) return reject();
    auto balance = math::subtract(account->credits, prepared.price), delta = math::negate(prepared.price);
    if (!balance || !delta) { failed = true; return; }
    // WGCH-29/30: the structure leaves first; payment, history and the actor follow only after removal.
    const auto structure = *found;
    release_structure_object(row, structure);
    std::erase_if(row.objects, [&](const auto& value) { return value.id == structure.id; });
    std::erase_if(control->planets[index].transition_modifiers, [&](const auto& m) { return m.source == structure.id; });
    dirty[static_cast<std::size_t>(StateSection::production)] = true;
    dirty[static_cast<std::size_t>(StateSection::control)] = true;
    population_stale = true;
    invalidate_observer_projection();
    const std::array<EntityId, 1> gone{structure.id};
    detach_observer_sources(gone);
    SabotageHistory updated{prepared.planet, (history ? history->count : 0U) + 1U};
    put(ability_host->sabotage_history, updated, [](const auto& r) { return r.planet; });
    if (credit_notification_handler) {
        auto notice = credit_notification_handler({CreditNotification::Kind::windfall, actor.owner, balance.value(), delta.value(), {}});
        if (!notice) { failed = true; return; }
    }
    account->credits = balance.value(); dirty[static_cast<std::size_t>(StateSection::players)] = true;
    auto consumed = ability_consumption_handler(*defeat);
    if (!consumed) { failed = true; return; }
    if (auto* retained = find(ability_host->preparations, actor.id, [](const auto& p) { return p.actor; })) retained->open = false;
    AbilityResult result{ability_host->next_request++, prepared.preparation, actor.id, prepared.planet,
        actor.owner, 0, AbilityOutcome::purchased};
    result.kind = AbilityKind::structure_sabotage;
    put(ability_host->results, std::move(result), [](const auto& p) { return p.actor; });
    touch_abilities();
    event.kind = GalacticEventKind::ability_settled; event.value = prepared.preparation; events.push_back(event);
}
} // namespace eawr::sim::galactic
