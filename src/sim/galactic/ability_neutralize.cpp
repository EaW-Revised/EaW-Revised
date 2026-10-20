#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "eawr/sim/galactic/economy_corruption.hpp"
#include "eawr/sim/galactic/income.hpp"

#include <algorithm>
#include <limits>
#include <utility>

// WGCH-20..26/52/63/91..95/131..134: live hero neutralization on the shared ability host. Preparation enumerates the
// visible hostile heroes at the planet, prices each (WGCH-22/23) and samples its two absence times (WGCH-21/93);
// execution resolves actor and victim again and applies WGCH-24..26/92/94.
// Named gaps: the planet protection ledger (empty), the active credit siphon target (its company/source metadata is
// not live), generic-hero visibility from sensors, final-blow scoring attribution, and local-player feedback text.
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
// WGCH-21: a nonpositive endpoint selects the default without consuming a draw; otherwise the range is sampled.
core::Result<Fixed> absence_sample(EconomyCorruptionState& state, std::uint64_t seed, Fixed low, Fixed high, Fixed fallback) {
    if (low.raw() <= 0 || high.raw() <= 0) return core::Result<Fixed>::success(fallback);
    if (low > high) return core::Result<Fixed>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, "invalid neutralization absence range"));
    auto draw = economy_draw(state, seed);
    if (!draw) return draw;
    auto width = math::subtract(high, low);
    if (!width) return width;
    auto scaled = math::multiply(width.value(), draw.value());
    if (!scaled) return scaled;
    return math::add(low, scaled.value());
}
}

void GalacticSession::Impl::prepare_neutralization(const GameCommand& command, const CensusObject& actor,
    const std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto planet = std::get<PrepareGalacticAbility>(command.payload).planet;
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    const auto* profile = find(setup.ability_host->profiles, actor.current_type, [](const auto& r) { return r.type; });
    const auto* service = find(ability_host->actors, actor.id, [](const auto& r) { return r.actor; });
    if (!profile || !profile->neutralize || !service || !service->neutralize_enabled || service->neutralize_cancelled
        || !control || !economy_corruption || !economy || !production || !planets.contains(planet)) return reject();
    const auto& names = setup.ability_host->activated_neutralize_abilities;
    if (std::find(names.begin(), names.end(), profile->neutralize->name) == names.end()) return reject();
    const auto* surface = find(control->planets, planet, [](const auto& r) { return r.planet; });
    const auto* economic = find(economy_corruption->planets, planet, [](const auto& r) { return r.planet; });
    if (!surface || !economic || !find(economy->players, actor.owner, [](const auto& p) { return p.player; })) return reject();
    // WGCH-20: a planet with a hackable superweapon is not a neutralization target.
    if (find_hack_weapon(planet)) return reject();
    const auto index = static_cast<std::size_t>(surface - control->planets.data());
    const auto& ability = *profile->neutralize;
    const auto controller = control->state[index].current_player;
    const auto allied = [&](PlayerId a, PlayerId b) {
        if (a == b) return true;
        for (const auto& relation : control->relations) if (relation.player == a && relation.other == b && relation.allied) return true;
        return false;
    };
    const auto is_transport = [&](TypeId type) {
        const auto* traits = movement_inputs ? find(movement_inputs->types, type, [](const auto& r) { return r.type; }) : nullptr;
        return traits && traits->transport;
    };
    // Container contents exist only with the movement owner; without it every object is a leaf.
    const auto contents_of = [&](EntityId id) -> const MovementContents* {
        return movement_inputs ? find(movement_inputs->contents, id, [](const auto& r) { return r.container; }) : nullptr;
    };
    const auto is_hero = [&](TypeId type) {
        return find(setup.ability_host->profiles, type, [](const auto& r) { return r.type; })
            || find(setup.ability_host->neutralization_targets, type, [](const auto& r) { return r.type; });
    };
    const auto live = [&](EntityId id) -> const CensusObject* {
        const auto at = actor_records.find(id);
        return at == actor_records.end() || at->second.dead || at->second.delete_pending ? nullptr : &at->second;
    };
    // WGCH-63: the first hero inside a transport represents it; fleets and squadrons recurse; a hero is itself.
    struct Candidate { EntityId id{}; bool orbit{}; };
    std::vector<Candidate> candidates; std::set<EntityId> visited;
    const auto flatten = [&](auto&& self, EntityId id, std::vector<EntityId>& out, std::size_t depth) -> void {
        if (depth > galactic_max_objects || !visited.insert(id).second) return;
        out.push_back(id);
        if (const auto* contents = contents_of(id))
            for (const auto& child : contents->children) self(self, child.id, out, depth + 1);
    };
    const auto gather = [&](auto&& self, EntityId id, bool orbit, std::size_t depth) -> void {
        const auto* row = live(id);
        if (!row || depth > galactic_max_objects) return;
        const auto* contents = contents_of(id);
        if (is_transport(row->current_type)) {
            std::vector<EntityId> inside;
            if (contents) for (const auto& child : contents->children) flatten(flatten, child.id, inside, 0);
            for (const auto inner : inside) if (const auto* hero = live(inner); hero && is_hero(hero->current_type)) {
                candidates.push_back({inner, orbit}); break;
            }
        } else if (contents) {
            for (const auto& child : contents->children) self(self, child.id, orbit, depth + 1);
        } else if (is_hero(row->current_type)) candidates.push_back({id, orbit});
    };
    for (const auto& fleet : sorted_fleets()) {
        if (fleet.orbit != planet || fleet.motion.transit || fleet.owner == actor.owner) continue;
        for (const auto& member : fleet.members) gather(gather, member.id, true, 0);
    }
    for (const auto& force : surface->landed) gather(gather, force.id, false, 0);
    AbilityPreparation prepared; prepared.actor = actor.id; prepared.planet = planet;
    prepared.kind = AbilityKind::hero_neutralization; prepared.open = true;
    const auto corrupted = economic->corruption != corruption_fixture::Type::none ? 1 : 0;
    const auto actor_original = find(setup.ability_host->profiles, actor.original_type, [](const auto& r) { return r.type; });
    const bool actor_named = actor_original && actor_original->named;
    for (const auto& candidate : candidates) {
        const auto* hero = live(candidate.id);
        if (!hero || allied(actor.owner, hero->owner)) continue;
        const auto* target = find(setup.ability_host->neutralization_targets, hero->current_type, [](const auto& r) { return r.type; });
        if (!target || target->cost.raw() <= 0 || !hero->current_type) continue;
        const auto* container = hero->container ? live(hero->container) : nullptr;
        const auto company = container && is_transport(container->current_type) ? container->original_type : hero->original_type;
        if (!company) continue;
        const auto victim_profile = find(setup.ability_host->profiles, hero->original_type, [](const auto& r) { return r.type; });
        const bool named = victim_profile && victim_profile->named;
        // WGCH-93: both absence samples are taken for every resolvable option, before any validity or price filter.
        auto mine = absence_sample(*economy_corruption, setup.seed, ability.actor_absence_min, ability.actor_absence_max,
            setup.ability_host->default_hero_absence);
        if (!mine) { failed = true; return; }
        auto theirs = absence_sample(*economy_corruption, setup.seed, ability.target_absence_min, ability.target_absence_max,
            setup.ability_host->default_hero_absence);
        if (!theirs) { failed = true; return; }
        dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
        // WGCH-22/23: base cost, base-level and history and corruption curves; the protection ledger is empty (named gap).
        auto base = math::multiply(target->cost, named ? ability.major_modifier : ability.minor_modifier);
        if (!base) { failed = true; return; }
        const auto level = allied(hero->owner, controller)
            ? static_cast<std::uint32_t>(candidate.orbit ? surface->starbase.level : surface->ground_base.level) : 0U;
        const auto* history = find(ability_host->neutralization_history, std::pair{company, actor.owner},
            [](const auto& r) { return std::pair{r.company, r.attacker}; });
        const auto count = history ? history->count : 0U;
        auto level_value = Fixed::from_integer(level), count_value = Fixed::from_integer(count);
        if (!level_value || !count_value) { failed = true; return; }
        auto from_base = evaluate_income_curve(ability.base_curve, level_value.value());
        auto from_history = evaluate_income_curve(ability.history_curve, count_value.value());
        auto from_corruption = evaluate_income_curve(ability.corruption_curve, Fixed::from_integer(corrupted).value());
        if (!from_base || !from_history || !from_corruption) { failed = true; return; }
        auto by_level = math::multiply(base.value(), from_base.value());
        auto by_history = math::multiply(base.value(), from_history.value());
        auto by_corruption = math::multiply(base.value(), from_corruption.value());
        if (!by_level || !by_history || !by_corruption) { failed = true; return; }
        auto sum = math::add(base.value(), by_level.value());
        if (!sum) { failed = true; return; }
        Fixed total = sum.value();
        if (named) {
            auto added = math::add(total, by_history.value());
            if (!added) { failed = true; return; }
            total = added.value();
        }
        auto reduced = math::subtract(total, by_corruption.value());
        if (!reduced) { failed = true; return; }
        const bool valid = (named ? ability.can_major : ability.can_minor) && (actor_named ? target->major_allowed : target->minor_allowed);
        prepared.target_choices.push_back({hero->id, company, std::max(corruption_fixture::one, reduced.value()),
            mine.value(), theirs.value(), valid});
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

void GalacticSession::Impl::execute_neutralization(const GameCommand& command, const CensusObject& actor,
    const PreparedAbility& prepared, const std::uint64_t tick, std::vector<GalacticEvent>& events) {
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    if (!economy || !ability_consumption_handler || !planets.contains(prepared.planet)) return reject();
    auto* account = find(economy->players, actor.owner, [](const auto& p) { return p.player; });
    const auto* original = find(setup.ability_host->profiles, actor.original_type, [](const auto& r) { return r.type; });
    const auto* content = find(setup.ability_host->theft_planets, prepared.planet, [](const auto& r) { return r.planet; });
    if (!account || !original) return reject();
    // WGCH-91/92: the stored planet resolves; the actor is the current owner's; the victim is resolved again by id.
    const auto victim_at = actor_records.find(prepared.target);
    if (victim_at == actor_records.end() || victim_at->second.dead || victim_at->second.delete_pending) return reject();
    const auto victim = victim_at->second;
    if (!prepared.valid) return reject();
    if (account->credits < prepared.price) return reject();
    bool captured = false;
    if (!original->named) {
        // WGCH-25: the planet flag intercepts a generic actor; an absent flag stays a named provider seam.
        if (!content || !content->auto_capture_minor) return reject();
        captured = *content->auto_capture_minor;
    }
    if (captured) {
        auto defeat = prepare_ability_defeat(actor, Fixed{}, true);
        if (!defeat) return reject();
        auto consumed = ability_consumption_handler(*defeat);
        if (!consumed) { failed = true; return; }
        if (auto* retained = find(ability_host->preparations, actor.id, [](const auto& p) { return p.actor; })) retained->open = false;
        AbilityResult result{ability_host->next_request++, prepared.preparation, actor.id, prepared.planet, actor.owner, 0, AbilityOutcome::refused};
        result.kind = AbilityKind::hero_neutralization;
        put(ability_host->results, std::move(result), [](const auto& p) { return p.actor; });
        touch_abilities();
        event.kind = GalacticEventKind::ability_settled; event.value = prepared.preparation; events.push_back(event); return;
    }
    // WGCH-26/94: the victim's company for the history is its immediate transport's original type, else its current type.
    const auto container = victim.container ? actor_records.find(victim.container) : actor_records.end();
    const auto* traits = container != actor_records.end() && movement_inputs
        ? find(movement_inputs->types, container->second.current_type, [](const auto& r) { return r.type; }) : nullptr;
    const auto company = traits && traits->transport ? container->second.original_type : victim.current_type;
    const auto* history = find(ability_host->neutralization_history, std::pair{company, actor.owner},
        [](const auto& r) { return std::pair{r.company, r.attacker}; });
    if (history && history->count == std::numeric_limits<std::uint32_t>::max()) return reject();
    auto victim_defeat = prepare_ability_defeat(victim, prepared.target_absence, true);
    auto actor_defeat = prepare_ability_defeat(actor, prepared.absence, true);
    if (!victim_defeat || !actor_defeat) return reject();
    auto balance = math::subtract(account->credits, prepared.price), delta = math::negate(prepared.price);
    if (!balance || !delta) { failed = true; return; }
    auto removed = ability_consumption_handler(*victim_defeat);
    if (!removed) { failed = true; return; }
    NeutralizationHistory updated{company, actor.owner, (history ? history->count : 0U) + 1U};
    put(ability_host->neutralization_history, updated, [](const auto& r) { return std::pair{r.company, r.attacker}; });
    if (credit_notification_handler) {
        auto notice = credit_notification_handler({CreditNotification::Kind::windfall, actor.owner, balance.value(), delta.value(), {}});
        if (!notice) { failed = true; return; }
    }
    account->credits = balance.value(); dirty[static_cast<std::size_t>(StateSection::players)] = true;
    auto consumed = ability_consumption_handler(*actor_defeat);
    if (!consumed) { failed = true; return; }
    if (auto* retained = find(ability_host->preparations, actor.id, [](const auto& p) { return p.actor; })) retained->open = false;
    AbilityResult result{ability_host->next_request++, prepared.preparation, actor.id, prepared.planet, actor.owner, 0, AbilityOutcome::purchased};
    result.kind = AbilityKind::hero_neutralization;
    put(ability_host->results, std::move(result), [](const auto& p) { return p.actor; });
    touch_abilities();
    event.kind = GalacticEventKind::ability_settled; event.value = prepared.preparation; events.push_back(event);
}
} // namespace eawr::sim::galactic
