#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/observer.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"

#include <algorithm>
#include <limits>

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
}
// WGCH-56/70/108: unpriced, current-owner visibility insertion, then success-only consumption.
void GalacticSession::Impl::execute_spy(const GameCommand& command, const CensusObject& live,
    const GalacticAbilityProfile& profile, PlanetId target, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto actor = live; // the consumption callback removes canonical actor records
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    if (!profile.spy || !observer || !control || !ability_host) return reject();
    const auto* planet = find(control->planets, target, [](const auto& row) { return row.planet; });
    const auto* service = find(ability_host->actors, actor.id, [](const auto& row) { return row.actor; });
    if (!planet || !service || !ground_ability_enabled(profile, *service, AbilityKind::system_spy)) return reject();
    const auto index = static_cast<std::size_t>(planet - control->planets.data());
    const auto allegiance = control->state[index].target_player;
    if (allegiance == actor.owner || std::any_of(control->relations.begin(), control->relations.end(), [&](const auto& row) {
        return row.player == actor.owner && row.other == allegiance && row.allied;
    })) return reject();
    if (ability_host->next_request == std::numeric_limits<std::uint64_t>::max()
        || (ability_host->results.size() >= galactic_max_objects
            && !find(ability_host->results, actor.id, [](const auto& row) { return row.actor; }))) return reject();
    const auto& ability = *profile.spy;
    std::optional<HeroDefeat> defeat;
    if (ability.causes_despawn) {
        if (!ability_consumption_handler) return reject();
        defeat = prepare_ability_defeat(actor, {}, false);
        if (!defeat) return reject();
        // A consumed last passenger leaves no transport or fleet to fight. Include
        // its empty carrier in the ordinary loss commit; retain non-empty carriers.
        if (movement_inputs && actor.container) {
            const auto* contents = find(movement_inputs->contents, actor.container,
                [](const auto& row) { return row.container; });
            const auto parent = actor_records.find(actor.container);
            if (contents && contents->children.size() == 1 && contents->children.front().id == actor.id
                && parent != actor_records.end() && parent->second.combat_role == ActorCombatRole::transport)
                defeat->removed_objects.push_back(actor.container);
        }
    }
    // GFS-01/GFD-03: galaxy commands execute before mode service. Child modes retain
    // galactic orders in their queue, so this entry uses the active galaxy clock.
    ObserverCommand effect; effect.action = ObserverAction::add; effect.player = actor.owner; effect.planet = target;
    effect.gameplay_frame = clock.galactic_frame; effect.logical_fps = observer->logical_fps;
    effect.spy_effect = true; // WGCH-108/WGUI-21: refresh every observer after insertion.
    effect.source = {actor.id, ability.identity, ability.text_identity, ability.mask, 0}; effect.duration = ability.duration;
    const auto begin = events.size(); apply_observer({command.key, effect}, tick, events);
    const bool success = std::any_of(events.begin() + static_cast<std::ptrdiff_t>(begin), events.end(), [](const auto& row) {
        return row.kind == GalacticEventKind::command_executed;
    });
    // WGCH-108: registration follows the attempt, including insertion refusal.
    auto* retained = find(ability_host->actors, actor.id, [](const auto& row) { return row.actor; });
    if (!retained) { failed = true; return; }
    const auto at = std::lower_bound(retained->spy_targets.begin(), retained->spy_targets.end(), target);
    if (at == retained->spy_targets.end() || *at != target) retained->spy_targets.insert(at, target);
    if (success && ground_ability_story_handler) {
        const auto told = ground_ability_story_handler(actor.owner, target, ability.text_identity, ability.hero);
        if (!told) { failed = true; return; }
    }
    if (success && defeat) {
        consuming_spy = actor.id;
        const auto consumed = ability_consumption_handler(*defeat);
        consuming_spy = 0;
        if (!consumed) { failed = true; return; }
    }
    AbilityResult result{ability_host->next_request++, 0, actor.id, target, actor.owner, 0,
        success ? AbilityOutcome::purchased : AbilityOutcome::refused};
    result.kind = AbilityKind::system_spy;
    put(ability_host->results, std::move(result), [](const auto& row) { return row.actor; });
    touch_abilities(); ++ability_work.executions;
    event.kind = GalacticEventKind::ability_settled; events.push_back(event);
}
void GalacticSession::Impl::prepare_theft(const GameCommand& command, const CensusObject& actor,
    const GalacticAbilityProfile& profile, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto target = std::get<PrepareGalacticAbility>(command.payload).planet;
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    if (!profile.theft || !production || !control || !economy) return reject();
    const auto* planet = find(control->planets, target, [](const auto& p) { return p.planet; });
    const auto* content = find(setup.ability_host->theft_planets, target, [](const auto& p) { return p.planet; });
    if (!planet || !content) return reject();
    const auto index = static_cast<std::size_t>(planet - control->planets.data());
    const auto& state = control->state[index];
    bool allied = actor.owner == state.target_player;
    for (const auto& relation : control->relations)
        if (relation.player == actor.owner && relation.other == state.target_player) allied |= relation.allied;
    const auto* owner = find(economy_corruption->players, state.current_player, [](const auto& p) { return p.player; });
    const auto* player = find(production->players, actor.owner, [](const auto& p) { return p.player; });
    const auto* account = find(economy->players, actor.owner, [](const auto& p) { return p.player; });
    const auto* purchaser = find(economy_corruption->players, actor.owner, [](const auto& p) { return p.player; });
    if (allied || !owner || !owner->playable || !player || !account || !purchaser) return reject();
    const auto& ability = *profile.theft;
    auto availability = evaluate_income_curve(ability.availability, content->availability);
    auto difficulty = evaluate_income_curve(ability.difficulty, content->difficulty);
    auto ground = Fixed::from_integer(planet->ground_base.level), space = Fixed::from_integer(planet->starbase.level);
    if (!availability || !difficulty || !ground || !space) { failed = true; return; }
    auto ground_chance = evaluate_income_curve(ability.ground, ground.value());
    auto space_chance = evaluate_income_curve(ability.space, space.value());
    if (!ground_chance || !space_chance) { failed = true; return; }
    AbilityPreparation prepared; prepared.actor = actor.id; prepared.planet = target;
    prepared.kind = AbilityKind::technology_theft; prepared.open = true;
    // WGCH-54: registration order, exact current player affiliation and locks.
    for (const auto& type : setup.ability_host->technologies) {
        ++ability_work.registered_types;
        const auto* registered = find(production->technology_types, type.type, [](const auto& t) { return t.type; });
        if (!registered || !registered->initially_locked || !registered->slicer_unlockable || registered->tech > account->current_tech
            || !std::binary_search(registered->affiliated_players.begin(), registered->affiliated_players.end(), actor.owner)
            || std::binary_search(player->locked.begin(), player->locked.end(), type.type)
            || std::binary_search(player->unlocked.begin(), player->unlocked.end(), type.type)) continue;
        const bool affinity = std::binary_search(content->affinity_types.begin(), content->affinity_types.end(), type.type);
        auto factor = math::add(availability.value(), affinity ? ability.affinity_price : Fixed{});
        if (!factor) { failed = true; return; }
        auto price = math::multiply(type.price, factor.value());
        if (!price) { failed = true; return; }
        price = math::multiply(price.value(), purchaser->tech_purchase_multiplier);
        auto chance = math::add(Fixed::from_raw(Fixed::scale), type.space ? space_chance.value() : ground_chance.value());
        if (!chance || !price) { failed = true; return; }
        chance = math::add(chance.value(), difficulty.value());
        if (!chance) { failed = true; return; }
        chance = math::add(chance.value(), affinity ? ability.affinity_chance : Fixed{});
        if (!chance) { failed = true; return; }
        prepared.theft_choices.push_back({type.type, std::max(Fixed{}, price.value()),
            std::clamp(chance.value(), Fixed{}, Fixed::from_raw(Fixed::scale)), {}, false});
    }
    // WGCH-101: ordinary empty-list admission cannot grant free advancement.
    if (prepared.theft_choices.empty()) return reject();
    Fixed absence;
    if (account->current_tech >= 0 && account->current_tech < 5
        && static_cast<std::size_t>(account->current_tech) < ability.absence_min.size()) {
        const auto slot = static_cast<std::size_t>(account->current_tech);
        auto draw = economy_draw(*economy_corruption, setup.seed);
        if (!draw) { failed = true; return; }
        auto width = math::subtract(ability.absence_max[slot], ability.absence_min[slot]);
        if (!width) { failed = true; return; }
        auto scaled = math::multiply(width.value(), draw.value());
        if (!scaled) { failed = true; return; }
        auto sampled = math::add(ability.absence_min[slot], scaled.value());
        if (!sampled) { failed = true; return; }
        absence = sampled.value(); dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
    for (auto& choice : prepared.theft_choices) { choice.absence = absence; choice.advance = prepared.theft_choices.size() == 1; }
    prepared.id = ability_host->next_request++;
    if (auto* menu = find(economy_corruption->menus, actor.id, [](const auto& m) { return m.buyer; }); menu && menu->active) {
        menu->active = false; dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
    event.kind = GalacticEventKind::ability_prepared; event.value = prepared.id;
    put(ability_host->preparations, std::move(prepared), [](const auto& p) { return p.actor; });
    touch_abilities(); events.push_back(event);
}
void GalacticSession::Impl::execute_theft(const GameCommand& command, const CensusObject& actor,
    const PreparedAbility& prepared, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    if (!economy || !production || !ability_consumption_handler || !planets.contains(prepared.planet)) return reject();
    auto* account = find(economy->players, actor.owner, [](const auto& p) { return p.player; });
    auto* player = find(production->players, actor.owner, [](const auto& p) { return p.player; });
    const auto* original = find(setup.ability_host->profiles, actor.original_type, [](const auto& p) { return p.type; });
    const auto* target = find(setup.ability_host->theft_planets, prepared.planet, [](const auto& p) { return p.planet; });
    const auto* technology = find(production->technology_types, prepared.theft.technology, [](const auto& t) { return t.type; });
    if (!account || !player || !original || !target || !technology || account->credits < prepared.price) return reject();
    // WGCH-100: no fresh affiliation, locks, tech, opposing or price filters.
    if (!original->named && !target->auto_capture_minor) return reject(); // omission default remains a named provider seam
    const bool captured = !original->named && *target->auto_capture_minor;
    auto defeat = prepare_ability_defeat(actor, captured ? Fixed{} : prepared.absence, true);
    if (!defeat) return reject();
    bool success = false, pay = false;
    if (!captured) {
        auto draw = economy_draw(*economy_corruption, setup.seed);
        if (!draw) { failed = true; return; }
        // WGCH-34/35: synchronized bounded choices; counter-stream equivalence
        // remains the same explicit fidelity seam as the market owner.
        const auto outcome = draw.value().raw() * 1000 / Fixed::scale;
        auto sampled = Fixed::from_ratio(outcome, 1000);
        if (!sampled) { failed = true; return; }
        success = sampled.value() < prepared.theft.chance; pay = success;
        if (!success) {
            auto failure = economy_draw(*economy_corruption, setup.seed);
            if (!failure) { failed = true; return; }
            const auto branch = 1 + failure.value().raw() * 3 / Fixed::scale;
            pay = branch == 3;
            if (branch == 1) {
                auto doubled = math::add(defeat->seconds, defeat->seconds);
                if (!doubled) { failed = true; return; }
                defeat->seconds = doubled.value();
            }
        }
        dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
    if (pay) {
        auto balance = math::subtract(account->credits, prepared.price), delta = math::negate(prepared.price);
        if (!balance || !delta) { failed = true; return; }
        if (credit_notification_handler) {
            auto notice = credit_notification_handler({CreditNotification::Kind::windfall, actor.owner, balance.value(), delta.value(), {}});
            if (!notice) { failed = true; return; }
        }
        account->credits = balance.value(); dirty[static_cast<std::size_t>(StateSection::players)] = true;
    }
    if (success) {
        const auto at = std::lower_bound(player->unlocked.begin(), player->unlocked.end(), prepared.theft.technology);
        if (at == player->unlocked.end() || *at != prepared.theft.technology) player->unlocked.insert(at, prepared.theft.technology);
        dirty[static_cast<std::size_t>(StateSection::production)] = true;
        if (prepared.theft.advance && account->current_tech < account->maximum_tech) {
            auto advanced = assign_technology({actor.owner, account->current_tech + 1, true, true});
            if (!advanced) { failed = true; return; }
        }
    }
    // WGCH-102: false ordinary outcomes also consume; scheduling precedes loss.
    auto consumed = ability_consumption_handler(*defeat);
    if (!consumed) { failed = true; return; }
    auto* retained = find(ability_host->preparations, actor.id, [](const auto& p) { return p.actor; });
    retained->open = false;
    AbilityResult result{ability_host->next_request++, prepared.preparation, actor.id, prepared.planet,
        actor.owner, 0, success ? AbilityOutcome::purchased : AbilityOutcome::refused};
    result.kind = AbilityKind::technology_theft; result.technology = success ? prepared.theft.technology : 0;
    put(ability_host->results, std::move(result), [](const auto& p) { return p.actor; });
    touch_abilities();
    event.kind = GalacticEventKind::ability_settled; event.value = prepared.preparation; events.push_back(event);
}
} // namespace eawr::sim::galactic
