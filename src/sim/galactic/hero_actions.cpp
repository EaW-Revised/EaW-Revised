#include "eawr/sim/galactic/hero_actions.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic::hero_actions {
namespace {
core::Diagnostic invalid(const char* message) {
    core::Diagnostic d; d.code = "EAWR-GC-HERO-ACTIONS-0001"; d.message = message; return d;
}
template<class T> auto* find(std::vector<T>& values, Id id) {
    const auto it = std::find_if(values.begin(), values.end(), [id](const auto& v) { return v.id == id && v.present; });
    return it == values.end() ? nullptr : &*it;
}
template<class T> const auto* find(const std::vector<T>& values, Id id) {
    const auto it = std::find_if(values.begin(), values.end(), [id](const auto& v) { return v.id == id && v.present; });
    return it == values.end() ? nullptr : &*it;
}
template<class W> auto* account(W& world, Id id) {
    const auto it = std::find_if(world.accounts.begin(), world.accounts.end(), [id](const auto& a) { return a.base.player.id == id; });
    return it == world.accounts.end() ? nullptr : &*it;
}
bool allied(const Fixture& w, Id a, Id b) {
    const auto* p = account(w, a); return a == b || (p && p->base.allies.contains(b));
}
Outcome refused(Refusal r) { Outcome result; result.refusal = r; return result; }
void emit(Outcome& r, Effect e, Id object = 0, Id player = 0, Id type = 0, Fixed value = {}, bool bypass = false, Id source = 0) {
    r.events.push_back({e, object, player, type, value, bypass, source});
}
// Diagnostic arithmetic is evaluated before associated gameplay mutations.
struct Arithmetic {
    bool ok{true};
    Fixed add(Fixed a, Fixed b) { Fixed v; ok = math::try_add(a, b, v) && ok; return v; }
    Fixed sub(Fixed a, Fixed b) { Fixed v; ok = math::try_subtract(a, b, v) && ok; return v; }
    Fixed mul(Fixed a, Fixed b) { Fixed v; ok = math::try_multiply(a, b, v) && ok; return v; }
};
core::Result<Fixed> sample(Range range, Fixed draw) {
    if (range.low > range.high || draw < Fixed{} || draw > one)
        return core::Result<Fixed>::failure(invalid("invalid synchronized range sample"));
    Arithmetic a; auto v = a.add(range.low, a.mul(a.sub(range.high, range.low), draw));
    return a.ok ? core::Result<Fixed>::success(v) : core::Result<Fixed>::failure(invalid("range sample overflow"));
}
Refusal opposing(const Fixture& w, const Object* actor, const Planet* p, bool playable) {
    if (!actor) return Refusal::actor;
    if (!p || !p->planetary) return Refusal::planet;
    if (allied(w, actor->owner, p->model.target_allegiance)) return Refusal::opposing;
    if (playable) {
        const auto* controller = account(w, p->current_allegiance);
        if (!controller || !controller->playable) return Refusal::playable;
    }
    return Refusal::none;
}
void schedule(Outcome& r, const Object& o, Fixed seconds, bool bypass) {
    emit(r, Effect::schedule, o.id, o.owner, o.current_type, seconds, bypass);
}
// WGCH-95: destruction of companions belongs to the transport lifecycle host.
void remove(Fixture& w, Object& o, Outcome& r, bool parent) {
    const auto parent_id = o.parent; o.present = false;
    emit(r, Effect::remove_object, o.id, o.owner, o.current_type); r.consumed = true;
    if (parent) if (auto* transport = find(w.objects, parent_id); transport && transport->transport) {
        transport->present = false; emit(r, Effect::remove_transport, transport->id, transport->owner, transport->original_type);
    }
}
void capture(Fixture& w, Object& o, Outcome& r, bool shared_removal) {
    schedule(r, o, {}, shared_removal); remove(w, o, r, shared_removal); r.refusal = Refusal::captured;
}
void flatten(const Fixture& w, Id id, std::vector<const Object*>& out, std::set<Id>& visited) {
    if (!visited.insert(id).second) return;
    const auto* o = find(w.objects, id); if (!o) return;
    out.push_back(o);
    for (auto child : o->contents) flatten(w, child, out, visited);
}
const Object* weapon(const Fixture& w, const Planet& p, Id type) {
    if (!type) return nullptr;
    std::vector<const Object*> objects; std::set<Id> visited;
    for (auto id : p.orbit) flatten(w, id, objects, visited);
    const auto it = std::find_if(objects.begin(), objects.end(), [type](const auto* o) { return o->current_type == type; });
    return it == objects.end() ? nullptr : *it;
}
// WGCH-63: the first hero inside a transport wins before outer filtering.
void candidates(const Fixture& w, Id id, std::vector<const Object*>& out, std::set<Id>& visited) {
    if (!visited.insert(id).second) return;
    const auto* o = find(w.objects, id); if (!o) return;
    if (o->transport) {
        std::vector<const Object*> inside; std::set<Id> child_visited;
        for (auto child : o->contents) flatten(w, child, inside, child_visited);
        const auto it = std::find_if(inside.begin(), inside.end(), [](const auto* v) { return v->hero; });
        if (it != inside.end()) out.push_back(*it);
    } else if (o->fleet) {
        for (auto child : o->contents) candidates(w, child, out, visited);
    } else if (o->hero) out.push_back(o);
}
void ruins(Fixture& w, Planet& p, Outcome& r, bool galactic_mode) {
    // WGCH-67: shared helper does not touch orbital fleets or starbase.
    emit(r, Effect::ruins, p.id);
    for (auto id : p.landed) if (auto* o = find(w.objects, id)) {
        o->present = false; emit(r, Effect::destroy_landed, id, o->owner);
    }
    p.landed.clear(); p.model.landed_owners.clear(); for (auto& s : p.ground) s.present = false;
    p.model.ground_specials = 0; emit(r, Effect::clear_ground, p.id);
    p.siphon.fiscal.active = false; emit(r, Effect::end_siphon, p.id);
    p.model.political_modifiers.clear(); p.political_sources.clear(); emit(r, Effect::clear_political, p.id);
    p.land_capacity = 0; p.destroyed = true; emit(r, Effect::destroyed_story, p.id);
    const auto old = p.model.transition.current;
    p.model.transition = {}; emit(r, Effect::abort_corruption, p.id);
    emit(r, Effect::refresh_corruption_visuals, p.id);
    emit(r, Effect::corruption_cleanup, p.id, 0, static_cast<Id>(old));
    if (galactic_mode) emit(r, Effect::terminate_abilities, p.id);
}
} // namespace

core::Result<Fixed> absence(Range range, Fixed draw, Fixed default_seconds) {
    // WGCH-21: don't consume a draw for either nonpositive endpoint.
    if (range.low <= Fixed{} || range.high <= Fixed{}) return core::Result<Fixed>::success(default_seconds);
    return sample(range, draw);
}

core::Result<NeutralizationPreparation> prepare_neutralization(const Fixture& w,
    Id actor_id, Id planet_id, Id required_type, const NeutralizationAbility& ability,
    std::span<const std::pair<Fixed, Fixed>> draws) {
    NeutralizationPreparation result;
    const auto* actor = find(w.objects, actor_id); const auto* p = find(w.planets, planet_id);
    auto refuse = [&](Refusal r) { result.refusal = r; return core::Result<NeutralizationPreparation>::success(result); };
    if (!actor) return refuse(Refusal::actor);
    if (!account(w, actor->owner)) return refuse(Refusal::owner);
    if (!p || !p->planetary) return refuse(Refusal::planet);
    if (weapon(w, *p, required_type)) return refuse(Refusal::superweapon);
    if (!ability.base_curve || !ability.history_curve || !ability.corruption_curve)
        return core::Result<NeutralizationPreparation>::failure(invalid("missing authored neutralization curve evaluator"));
    std::vector<std::pair<const Object*, bool>> list;
    std::set<Id> visited;
    for (auto id : p->orbit) {
        const auto* root = find(w.objects, id); if (!root || !root->visible) continue;
        std::vector<const Object*> heroes; candidates(w, id, heroes, visited);
        for (const auto* hero : heroes) list.emplace_back(hero, true);
    }
    for (auto id : p->landed) {
        std::vector<const Object*> heroes; candidates(w, id, heroes, visited);
        for (const auto* hero : heroes) list.emplace_back(hero, false);
    }
    struct Target { Id id, owner, company; bool siphon, named, orbit, major, minor; Fixed cost; };
    std::vector<Target> targets;
    for (const auto& [hero, orbit] : list) {
        if (allied(w, actor->owner, hero->owner) || !hero->visible || hero->hidden_details || hero->neutralization_cost <= Fixed{}) continue;
        const auto* parent = find(w.objects, hero->parent);
        const auto company = parent && parent->transport ? parent->original_type : hero->original_type;
        if (hero->current_type && company) targets.push_back({hero->id, hero->owner, company, false,
            hero->named, orbit, hero->major_allowed, hero->minor_allowed, hero->neutralization_cost});
    }
    if (p->siphon.fiscal.active && p->siphon.company_type && p->siphon.source_type && p->siphon.neutralization_cost > Fixed{}
        && !allied(w, actor->owner, p->siphon.fiscal.recipient))
        targets.push_back({0, p->siphon.fiscal.recipient, p->siphon.company_type, true, p->siphon.named, false,
            p->siphon.major_allowed, p->siphon.minor_allowed, p->siphon.neutralization_cost});
    if (draws.size() != targets.size()) return core::Result<NeutralizationPreparation>::failure(invalid("neutralization requires one draw pair per candidate"));
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const auto& t = targets[i];
        auto at = absence(ability.actor_absence, draws[i].first, ability.default_absence);
        auto tt = absence(ability.target_absence, draws[i].second, ability.default_absence);
        if (!at) return core::Result<NeutralizationPreparation>::failure(at.error());
        if (!tt) return core::Result<NeutralizationPreparation>::failure(tt.error());
        Arithmetic a;
        const auto base = a.mul(t.cost, t.named ? ability.major_modifier : ability.minor_modifier);
        std::map<Id, Fixed> categories;
        for (const auto& m : p->protection) if (m.source != t.id || t.siphon) {
            auto [it, inserted] = categories.emplace(m.category, m.multiplier);
            if (!inserted) it->second = std::max(it->second, m.multiplier);
        }
        auto product = one; for (const auto& [category, multiplier] : categories) { (void)category; product = a.mul(product, multiplier); }
        const unsigned level = allied(w, t.owner, p->current_allegiance) ? (t.orbit ? p->model.starbase_level : p->ground_base) : 0;
        const auto history = w.neutralizations.find({t.company, actor->owner});
        const auto count = history == w.neutralizations.end() ? 0 : history->second;
        auto price = a.add(base, a.mul(base, ability.base_curve(level)));
        price = a.add(price, a.mul(base, a.sub(product, one)));
        if (t.named) price = a.add(price, a.mul(base, ability.history_curve(count)));
        price = a.sub(price, a.mul(base, ability.corruption_curve(p->model.transition.current != corruption_fixture::Type::none ? 1U : 0U)));
        if (!a.ok) return core::Result<NeutralizationPreparation>::failure(invalid("neutralization price overflow"));
        const bool valid = (t.named ? ability.major : ability.minor) && (actor->named ? t.major : t.minor);
        result.choices.push_back({actor_id, planet_id, t.id, t.company, t.siphon, valid,
            std::max(one, price), at.value(), tt.value()});
    }
    if (result.choices.empty()) result.refusal = Refusal::no_choices;
    return core::Result<NeutralizationPreparation>::success(std::move(result));
}

core::Result<Outcome> neutralize(Fixture& w, const Neutralization& choice) {
    auto* p = find(w.planets, choice.planet); if (!p || !p->planetary) return core::Result<Outcome>::success(refused(Refusal::planet));
    auto* actor = find(w.objects, choice.actor); if (!actor) return core::Result<Outcome>::success(refused(Refusal::actor));
    auto* player = account(w, actor->owner); if (!player) return core::Result<Outcome>::success(refused(Refusal::owner));
    auto* target = choice.siphon ? nullptr : find(w.objects, choice.target);
    if (choice.siphon ? !p->siphon.fiscal.active : !target) return core::Result<Outcome>::success(refused(Refusal::target));
    if (!choice.valid) return core::Result<Outcome>::success(refused(Refusal::invalid_option));
    const auto price = std::max(one, choice.price);
    if (player->base.player.credits < price) return core::Result<Outcome>::success(refused(Refusal::credits));
    auto balance = math::subtract(player->base.player.credits, price); if (!balance) return core::Result<Outcome>::failure(balance.error());
    Outcome r;
    if (!actor->named && p->auto_capture_minor) { capture(w, *actor, r, false); return core::Result<Outcome>::success(r); }
    const auto* target_parent = target ? find(w.objects, target->parent) : nullptr;
    const auto score_type = choice.siphon ? choice.company_type
        : target_parent && target_parent->transport ? target_parent->original_type : target->current_type;
    const auto history = w.neutralizations.find({score_type, actor->owner});
    if (history != w.neutralizations.end() && history->second == std::numeric_limits<unsigned>::max())
        return core::Result<Outcome>::failure(invalid("neutralization history overflow"));
    actor->final_blow = false;
    if (choice.siphon) {
        emit(r, Effect::neutralized, p->id, actor->owner, choice.company_type);
        ++w.neutralizations[{choice.company_type, actor->owner}];
        p->siphon.fiscal.active = false; emit(r, Effect::end_siphon, p->id, p->siphon.fiscal.recipient, p->siphon.source_type);
    } else {
        target->final_blow = true; emit(r, Effect::attribution, target->id, actor->owner, target->current_type);
        schedule(r, *target, choice.target_absence, true);
        const auto* parent = find(w.objects, target->parent);
        const auto type = parent && parent->transport ? parent->original_type : target->current_type;
        emit(r, Effect::neutralized, parent && parent->transport ? parent->id : target->id, actor->owner, type);
        ++w.neutralizations[{type, actor->owner}]; remove(w, *target, r, true);
    }
    player->base.player.credits = balance.value(); emit(r, Effect::debit, actor->id, actor->owner, 0, price);
    emit(r, Effect::arrival, actor->id, actor->owner); emit(r, Effect::tutorial, p->id, actor->owner);
    emit(r, Effect::presentation, p->id, actor->owner);
    schedule(r, *actor, choice.actor_absence, false); remove(w, *actor, r, true); r.success = true;
    return core::Result<Outcome>::success(std::move(r));
}

std::optional<Neutralization> select_neutralization_ai(const Fixture& w,
    const NeutralizationPreparation& prepared, unsigned index) {
    if (prepared.refusal != Refusal::none) return std::nullopt;
    for (const auto& c : prepared.choices) {
        const auto* actor = find(w.objects, c.actor); const auto* p = actor ? account(w, actor->owner) : nullptr;
        if (!c.valid || !p || p->base.player.credits < c.price) continue;
        if (index-- == 0) return c;
    }
    return std::nullopt;
}

core::Result<Fixed> sabotage_price(Fixed cost, Fixed factor, unsigned count) {
    if (cost < one) return core::Result<Fixed>::failure(invalid("WGCH-28 ordinary sabotage cost below one"));
    Arithmetic a; auto power = one;
    // Exponentiation by squaring bounds fixture work even for large histories.
    while (count) { if (count & 1U) power = a.mul(power, factor); count >>= 1U; if (count) factor = a.mul(factor, factor); }
    const auto price = a.mul(cost, power);
    if (!a.ok || price < Fixed{}) return core::Result<Fixed>::failure(invalid("WGCH-28 invalid escalating price"));
    auto shifted = math::add(price, Fixed::from_ratio(1, 2).value()); if (!shifted) return shifted;
    // WGCH-28 nearest integer; nonnegative ties round upward.
    return Fixed::from_integer(shifted.value().trunc_to_integer());
}
core::Result<std::vector<Sabotage>> prepare_sabotage(const Fixture& w, Id actor_id, Id planet_id, Fixed factor, bool tutorial) {
    const auto* actor = find(w.objects, actor_id); const auto* p = find(w.planets, planet_id);
    const auto gate = opposing(w, actor, p, false);
    if (gate != Refusal::none || !account(w, actor->owner) || p->model.transition.current == corruption_fixture::Type::none)
        return core::Result<std::vector<Sabotage>>::success({});
    std::vector<Sabotage> choices;
    for (const auto* layer : {&p->ground, &p->space}) for (const auto& s : *layer) {
        const auto cost = tutorial ? s.tutorial_cost : s.cost;
        if (!s.present || cost < one) continue;
        auto price = tutorial ? core::Result<Fixed>::success(cost) : sabotage_price(cost, factor, p->sabotage_count);
        if (!price) return core::Result<std::vector<Sabotage>>::failure(price.error());
        choices.push_back({actor_id, planet_id, s.id, price.value(), {}}); // WGCH-77 menu absence zero
    }
    return core::Result<std::vector<Sabotage>>::success(std::move(choices));
}
core::Result<bool> menu_affordable(Fixed price, Fixed credits) {
    auto rounded = math::add(credits, Fixed::from_ratio(1, 2).value());
    if (!rounded) return core::Result<bool>::failure(rounded.error());
    auto integer = Fixed::from_integer(rounded.value().trunc_to_integer());
    if (!integer) return core::Result<bool>::failure(integer.error());
    return core::Result<bool>::success(price <= integer.value());
}
core::Result<Outcome> sabotage(Fixture& w, const Sabotage& choice, SabotageRoute route) {
    if (choice.price < Fixed{}) return core::Result<Outcome>::success(refused(Refusal::cost));
    auto* p = find(w.planets, choice.planet); if (!p || !p->planetary) return core::Result<Outcome>::success(refused(Refusal::planet));
    const bool free = route == SabotageRoute::free_script;
    auto* actor = free ? nullptr : find(w.objects, choice.actor);
    if (!free && !actor) return core::Result<Outcome>::success(refused(Refusal::actor));
    auto* player = actor ? account(w, actor->owner) : nullptr;
    if (!free && !player) return core::Result<Outcome>::success(refused(Refusal::owner));
    if (!free && player->base.player.credits < choice.price) return core::Result<Outcome>::success(refused(Refusal::credits));
    const bool ground = find(p->ground, choice.structure) != nullptr;
    auto* s = ground ? find(p->ground, choice.structure) : find(p->space, choice.structure);
    if (!s) return core::Result<Outcome>::success(refused(Refusal::target));
    Arithmetic a;
    const auto balance = free ? Fixed{} : a.sub(player->base.player.credits, choice.price);
    const auto income = route == SabotageRoute::direct_ai ? a.sub(player->frame_credit_income, choice.price) : Fixed{};
    if (!a.ok || p->sabotage_count == std::numeric_limits<unsigned>::max())
        return core::Result<Outcome>::failure(invalid("sabotage arithmetic overflow"));
    Outcome r;
    if (s->removal_accepts) s->present = false;
    emit(r, ground ? Effect::reevaluate_ground : Effect::reevaluate_space, p->id, actor ? actor->owner : 0);
    if (!s->removal_accepts) { r.refusal = Refusal::removal; return core::Result<Outcome>::success(r); }
    emit(r, Effect::sabotage_story, s->id, actor ? actor->owner : 0);
    if (route == SabotageRoute::prepared) {
        player->base.player.credits = balance; emit(r, Effect::debit, actor->id, actor->owner, 0, choice.price);
        if (choice.absence >= Fixed{}) schedule(r, *actor, choice.absence, false);
        remove(w, *actor, r, true);
    } else if (route == SabotageRoute::direct_ai) {
        player->frame_credit_income = income; emit(r, Effect::frame_income, p->id, actor->owner, 0, choice.price);
    }
    if (route != SabotageRoute::direct_ai || p->revealed) emit(r, Effect::sabotage_effect, p->id);
    ++p->sabotage_count; r.success = true;
    return core::Result<Outcome>::success(std::move(r));
}
Outcome legacy_sabotage_capture(Fixture& w, Id actor_id, Id planet_id) {
    auto* actor = find(w.objects, actor_id); auto* p = find(w.planets, planet_id);
    if (!actor) return refused(Refusal::actor);
    if (!p || !p->planetary) return refused(Refusal::planet);
    Outcome r;
    if (!actor->named && p->auto_capture_minor) { capture(w, *actor, r, false); r.success = true; }
    return r; // U-07: mod-only legacy activity effects not implemented.
}
core::Result<Outcome> sabotage_ai(Fixture& w, Id actor_id, Id planet_id, Fixed factor, unsigned index) {
    const auto* actor = find(w.objects, actor_id); const auto* p = find(w.planets, planet_id);
    const auto gate = opposing(w, actor, p, false);
    if (gate != Refusal::none) return core::Result<Outcome>::success(refused(gate));
    if (p->model.transition.current == corruption_fixture::Type::none)
        return core::Result<Outcome>::success(refused(Refusal::no_choices));
    const auto present = [](const auto& s) { return s.present; };
    if (!std::any_of(p->ground.begin(), p->ground.end(), present) && !std::any_of(p->space.begin(), p->space.end(), present))
        return core::Result<Outcome>::success(refused(Refusal::no_choices));
    const auto* player = account(w, actor->owner); if (!player) return core::Result<Outcome>::success(refused(Refusal::owner));
    if (!actor->named && p->auto_capture_minor)
        return core::Result<Outcome>::success(legacy_sabotage_capture(w, actor_id, planet_id));
    std::vector<Sabotage> options;
    for (const auto* layer : {&p->ground, &p->space}) for (const auto& s : *layer) {
        if (!s.present || s.deletion_pending || s.cost < one) continue;
        auto price = sabotage_price(s.cost, factor, p->sabotage_count);
        if (!price) return core::Result<Outcome>::failure(price.error());
        if (price.value() <= player->base.player.credits) options.push_back({actor_id, planet_id, s.id, price.value(), {}});
    }
    if (index >= options.size()) return core::Result<Outcome>::success(refused(Refusal::no_choices));
    return sabotage(w, options[index], SabotageRoute::direct_ai);
}

core::Result<TheftPreparation> prepare_theft(const Fixture& w, Id actor_id, Id planet_id,
    std::span<const Technology> types, const TheftAbility& ability, Fixed draw) {
    TheftPreparation result;
    const auto* actor = find(w.objects, actor_id); const auto* p = find(w.planets, planet_id);
    result.refusal = opposing(w, actor, p, true);
    if (result.refusal != Refusal::none) return core::Result<TheftPreparation>::success(result);
    const auto* player = account(w, actor->owner);
    if (!player) { result.refusal = Refusal::owner; return core::Result<TheftPreparation>::success(result); }
    if (!ability.availability || !ability.ground || !ability.space || !ability.difficulty)
        return core::Result<TheftPreparation>::failure(invalid("missing authored theft curve evaluator"));
    for (const auto& t : types) {
        if (!t.id || t.faction != player->base.player.faction || t.tech > player->tech || !t.initially_locked
            || !t.slicer_unlock || player->unlocked.contains(t.id) || player->locked.contains(t.id)) continue;
        Arithmetic a; const bool affinity = ability.affinity_types.contains(t.id);
        auto price = a.mul(a.mul(t.cost, a.add(ability.availability(p->tech_rating), affinity ? ability.affinity_price : Fixed{})), ability.purchase_multiplier);
        const auto base = t.space ? ability.space(p->model.starbase_level) : ability.ground(p->ground_base);
        auto chance = a.add(a.add(one, base), ability.difficulty(p->slice_difficulty));
        if (affinity) chance = a.add(chance, ability.affinity_chance);
        if (!a.ok) return core::Result<TheftPreparation>::failure(invalid("theft price/chance overflow"));
        // WGCH-36 is the direct 0..4 tech index. The black-market helper
        // (WGCH-38/71) instead subtracts one from displayed tech; do not use it
        // with this raw index. Empty/out-of-range arrays request default time.
        auto timer = core::Result<Fixed>::success({});
        const auto index = static_cast<std::size_t>(player->tech);
        if (index < 5 && index < ability.absence_min.size() && index < ability.absence_max.size())
            timer = sample({ability.absence_min[index], ability.absence_max[index]}, draw);
        if (!timer) return core::Result<TheftPreparation>::failure(timer.error());
        result.choices.push_back({actor_id, planet_id, t.id, std::max(Fixed{}, price), std::clamp(chance, Fixed{}, one), timer.value(), false});
    }
    if (result.choices.empty()) result.refusal = Refusal::no_choices; // WGCH-101: no free advancement
    if (result.choices.size() == 1) result.choices[0].advance = true; // WGCH-64, entire prepared list
    return core::Result<TheftPreparation>::success(std::move(result));
}
core::Result<Outcome> steal(Fixture& w, const Theft& choice, Fixed draw, unsigned failure) {
    auto* p = find(w.planets, choice.planet); if (!p || !p->planetary) return core::Result<Outcome>::success(refused(Refusal::planet));
    auto* actor = find(w.objects, choice.actor); if (!actor) return core::Result<Outcome>::success(refused(Refusal::actor));
    auto* player = account(w, actor->owner); if (!player) return core::Result<Outcome>::success(refused(Refusal::owner));
    if (!choice.technology) return core::Result<Outcome>::success(refused(Refusal::target));
    if (choice.price < Fixed{} || choice.chance < Fixed{} || choice.chance > one)
        return core::Result<Outcome>::failure(invalid("invalid prepared theft"));
    if (player->base.player.credits < choice.price) return core::Result<Outcome>::success(refused(Refusal::credits));
    Outcome r;
    if (!actor->named && p->auto_capture_minor) { capture(w, *actor, r, true); return core::Result<Outcome>::success(r); }
    const bool success = draw < choice.chance;
    if (draw < Fixed{} || draw > Fixed::from_decimal("0.999").value() || (!success && (failure < 1 || failure > 3)))
        return core::Result<Outcome>::failure(invalid("invalid synchronized theft draw"));
    Arithmetic a; const auto timer = !success && failure == 1 ? a.add(choice.absence, choice.absence) : choice.absence;
    const bool pay = success || failure == 3;
    const auto balance = pay ? a.sub(player->base.player.credits, choice.price) : player->base.player.credits;
    if (!a.ok)
        return core::Result<Outcome>::failure(invalid("theft arithmetic overflow"));
    if (pay) { player->base.player.credits = balance; emit(r, Effect::debit, actor->id, actor->owner, 0, choice.price); }
    if (success) {
        player->unlocked.insert(choice.technology); emit(r, Effect::unlock, actor->id, actor->owner, choice.technology);
        // WGEP-148: maximum-level increment skips the setter but retains theft.
        if (choice.advance && player->tech < player->maximum_tech) {
            ++player->tech; emit(r, Effect::advance_tech, actor->id, actor->owner);
        }
    }
    schedule(r, *actor, timer, true); remove(w, *actor, r, true); r.success = success;
    return core::Result<Outcome>::success(std::move(r));
}
std::optional<Theft> select_theft_ai(const Fixture& w, const TheftPreparation& prepared, unsigned index) {
    if (prepared.refusal != Refusal::none) return std::nullopt;
    for (const auto& c : prepared.choices) {
        const auto* actor = find(w.objects, c.actor); const auto* p = actor ? account(w, actor->owner) : nullptr;
        if (!p || p->base.player.credits < c.price) continue;
        if (index-- == 0) return c;
    }
    return std::nullopt;
}

core::Result<Outcome> deploy_siphon(Fixture& w, Id actor_id, Id planet_id,
    const SiphonAbility& ability, Fixed draw, Frame now, Frame fiscal_frames) {
    auto* actor = find(w.objects, actor_id); auto* p = find(w.planets, planet_id);
    const auto gate = opposing(w, actor, p, true);
    if (gate != Refusal::none) return core::Result<Outcome>::success(refused(gate));
    if (!account(w, actor->owner)) return core::Result<Outcome>::success(refused(Refusal::owner));
    if (!actor->named && p->prevent_minor_siphon) return core::Result<Outcome>::success(refused(Refusal::prevented));
    if (p->siphon.fiscal.active) return core::Result<Outcome>::success(refused(Refusal::siphon));
    Outcome r;
    if (!actor->named && p->auto_capture_minor) { capture(w, *actor, r, false); return core::Result<Outcome>::success(r); }
    auto fraction = sample(ability.percentage, draw); if (!fraction) return core::Result<Outcome>::failure(fraction.error());
    const auto* transport = find(w.objects, actor->parent);
    if (ability.cycles && fiscal_frames > (std::numeric_limits<Frame>::max() - now) / ability.cycles)
        return core::Result<Outcome>::failure(invalid("siphon deadline overflow"));
    emit(r, Effect::hero_land, actor->id, actor->owner);
    if (!transport || !transport->transport || !transport->original_type) r.refusal = Refusal::transport;
    else {
        p->siphon.fiscal = {true, actor->owner, fraction.value(), now + fiscal_frames * ability.cycles};
        p->siphon.source_type = actor->current_type; p->siphon.company_type = transport->original_type;
        p->siphon.named = actor->named;
        // WGCH-20/52/105: target metadata is for this copied source type,
        // never an expired/replaced siphon's previous source type.
        p->siphon.neutralization_cost = actor->neutralization_cost;
        p->siphon.major_allowed = actor->major_allowed;
        p->siphon.minor_allowed = actor->minor_allowed;
        p->siphon.accumulated = {}; p->siphon.discovered = false; r.success = true;
        emit(r, Effect::ability_effect, p->id, actor->owner);
    }
    p->registered_siphon_targets[actor_id] = planet_id; emit(r, Effect::register_target, planet_id, actor->owner, actor_id);
    return core::Result<Outcome>::success(std::move(r));
}
Outcome remove_siphon(Fixture& w, Id actor_id, Id planet_id) {
    auto* actor = find(w.objects, actor_id); auto* p = find(w.planets, planet_id);
    const auto gate = opposing(w, actor, p, true); if (gate != Refusal::none) return refused(gate);
    Outcome r;
    r.success = p->siphon.fiscal.recipient == actor->owner;
    if (r.success) { p->siphon.fiscal.active = false; emit(r, Effect::end_siphon, p->id, actor->owner); }
    p->registered_siphon_targets.erase(actor_id); emit(r, Effect::deregister_target, p->id, actor->owner, actor_id);
    return r;
}
Outcome spy(Fixture& w, Id actor_id, Id planet_id, const Spy& ability) {
    auto* actor = find(w.objects, actor_id); auto* p = find(w.planets, planet_id);
    if (!actor) return refused(Refusal::actor);
    if (!p) return refused(Refusal::planet);
    const bool self = actor->planetary && actor_id == planet_id;
    if (!self && ability.activation == Activation::ground) {
        const auto gate = opposing(w, actor, p, false); if (gate != Refusal::none) return refused(gate);
    } else if (!self && actor->planetary && actor->destroyed) return refused(Refusal::destroyed);
    auto* owner = account(w, actor->owner); if (!owner) return refused(Refusal::owner);
    if (!p->planetary) return refused(Refusal::planet);
    Outcome r;
    if (ability.insertion_accepts) {
        const auto source = ability.unique_source ? ability.unique_source : actor_id;
        p->spy_sources.insert(source); // WGUI-23 appends
        r.success = true; emit(r, Effect::spy_insert, p->id, actor->owner, ability.mask, ability.duration, false, source);
        for (const auto& player : w.accounts) emit(r, Effect::refresh_visibility, p->id, player.base.player.id);
        if (ability.activation == Activation::ground) emit(r, Effect::hero_land, actor_id, actor->owner);
        emit(r, Effect::ability_effect, p->id, actor->owner);
    } else r.refusal = Refusal::visibility;
    p->registered_spy_targets.insert(actor_id); emit(r, Effect::register_target, p->id, actor->owner, actor_id);
    return r;
}
void consume_success(Fixture& w, Id actor_id, bool causes_despawn, Fixed timer, Outcome& r) {
    if (!r.success || !causes_despawn) return;
    if (auto* actor = find(w.objects, actor_id)) { schedule(r, *actor, timer, false); remove(w, *actor, r, false); }
}

core::Result<Outcome> hack(Fixture& w, const Hack& choice, HackRoute route,
    std::uint32_t fps, Frame now, Id neutral_player) {
    auto* actor = find(w.objects, choice.actor); if (!actor) return core::Result<Outcome>::success(refused(Refusal::actor));
    auto* p = find(w.planets, choice.planet); if (!p || !p->planetary) return core::Result<Outcome>::success(refused(Refusal::planet));
    const auto* match = weapon(w, *p, choice.required_type);
    if (!match) return core::Result<Outcome>::success(refused(Refusal::superweapon));
    auto* hacker = account(w, actor->owner); if (!hacker) return core::Result<Outcome>::success(refused(Refusal::owner));
    if (choice.prepared_price < Fixed{}) return core::Result<Outcome>::failure(invalid("negative prepared hack price"));
    auto removal = p->model.transition;
    bool begin_removal = false;
    if (removal.current != corruption_fixture::Type::none) {
        auto started = corruption_fixture::begin(removal, corruption_fixture::Type::none,
            Fixed::from_integer(5).value(), fps, now);
        if (!started) return core::Result<Outcome>::failure(started.error());
        begin_removal = started.value();
    }
    Outcome r;
    if (route == HackRoute::human_prepared) {
        if (hacker->base.player.credits < choice.prepared_price) return core::Result<Outcome>::success(refused(Refusal::credits));
        auto balance = math::subtract(hacker->base.player.credits, choice.prepared_price);
        if (!balance) return core::Result<Outcome>::failure(balance.error());
        hacker->base.player.credits = balance.value(); emit(r, Effect::debit, actor->id, actor->owner, 0, choice.prepared_price);
    }
    auto* weapon_owner = account(w, match->owner);
    if (!weapon_owner) { r.refusal = Refusal::owner; return core::Result<Outcome>::success(r); } // WGCH-66 after debit
    weapon_owner->locked.insert(match->current_type); emit(r, Effect::build_lock, match->id, match->owner, match->current_type);
    if (route == HackRoute::human_prepared) emit(r, Effect::hack_sound, p->id, actor->owner);
    remove(w, *actor, r, true); // WGCH-110: no explicit respawn scheduling
    if (begin_removal) {
        p->model.transition = removal;
        emit(r, Effect::corruption_removal, p->id, 0, 0, Fixed::from_integer(5).value());
    }
    ruins(w, *p, r, true); emit(r, Effect::refresh_model, p->id);
    for (auto id : p->orbit) if (auto* o = find(w.objects, id)) { o->present = false; emit(r, Effect::destroy_orbit, id, o->owner); }
    p->orbit.clear(); p->model.orbital_owners.clear(); p->model.starbase_level = 0; emit(r, Effect::clear_starbase, p->id);
    if (neutral_player && account(w, neutral_player)) {
        p->current_allegiance = neutral_player; p->model.target_allegiance = neutral_player;
        emit(r, Effect::neutral_ownership, p->id, neutral_player);
    }
    if (route == HackRoute::human_prepared) emit(r, Effect::hack_particle, p->id);
    r.success = true; return core::Result<Outcome>::success(std::move(r));
}
core::Result<Outcome> invoke_hack(Fixture& w, const Hack& choice,
    std::uint32_t fps, Frame now, Id neutral_player) {
    const auto* actor = find(w.objects, choice.actor); const auto* p = find(w.planets, choice.planet);
    if (!actor) return core::Result<Outcome>::success(refused(Refusal::actor));
    if (!p || !p->planetary) return core::Result<Outcome>::success(refused(Refusal::planet));
    if (!weapon(w, *p, choice.required_type)) return core::Result<Outcome>::success(refused(Refusal::superweapon));
    const auto* owner = account(w, actor->owner); if (!owner) return core::Result<Outcome>::success(refused(Refusal::owner));
    if (owner->base.ai && !owner->human) return hack(w, choice, HackRoute::direct_ai, fps, now, neutral_player);
    Outcome r;
    if (owner->base.local) emit(r, Effect::prepare_menu, p->id, actor->owner);
    else r.refusal = Refusal::remote;
    return core::Result<Outcome>::success(r);
}
Outcome destroy_planet(Fixture& w, Id actor_id, Id planet_id, bool galactic_mode) {
    auto* actor = find(w.objects, actor_id); auto* p = find(w.planets, planet_id);
    if (!actor) return refused(Refusal::actor);
    if (!p || !p->planetary) return refused(Refusal::planet);
    if (p->destroyed) return refused(Refusal::destroyed);
    if (account(w, p->model.target_allegiance) && allied(w, actor->owner, p->model.target_allegiance)) return refused(Refusal::opposing);
    if (actor->contested) for (auto id : p->orbit) {
        const auto* o = find(w.objects, id); if (o && o->fleet && !o->stealth && !allied(w, actor->owner, o->owner)) return refused(Refusal::contested);
    }
    Outcome r; const auto* attacker = account(w, actor->owner); const auto* controller = account(w, p->current_allegiance);
    if (galactic_mode && attacker && attacker->leader_kill_victory && controller && controller->base.player.faction && controller->leader_type) {
        bool leader = false;
        for (auto id : p->landed) if (const auto* o = find(w.objects, id); o && o->transport)
            for (auto child : o->contents) if (const auto* member = find(w.objects, child); member && member->current_type == controller->leader_type) leader = true;
        if (leader) emit(r, Effect::pending_victory, p->id, actor->owner, controller->leader_type, Fixed::from_integer(30).value());
    }
    ruins(w, *p, r, galactic_mode); emit(r, Effect::refresh_model, p->id); emit(r, Effect::ability_effect, p->id, actor->owner);
    r.success = true; return r;
}
Outcome political_protection(Fixture& w, Id source_id, Id planet_id, bool apply, Id unique_source, bool appropriate) {
    const auto* source = find(w.objects, source_id); auto* p = find(w.planets, planet_id);
    if (!source) return refused(Refusal::actor);
    if (!p || !p->planetary) return refused(Refusal::planet);
    if (!appropriate) return refused(Refusal::invalid_option);
    const auto key = unique_source ? unique_source : source_id;
    // WGCH-59/68: host supplies appropriation/owner-change callback admission;
    // this pure source-keyed modifier operation does not infer that predicate.
    Outcome r; r.success = true;
    if (apply) { p->political_sources.insert(key); emit(r, Effect::political_protection, p->id, source->owner, key, Fixed::from_integer(-1).value()); emit(r, Effect::register_target, p->id, source->owner, key); emit(r, Effect::ability_effect, p->id); }
    else { p->political_sources.erase(key); emit(r, Effect::deregister_target, p->id, source->owner, key); }
    return r;
}
Outcome galactic_stealth(Fixture& w, Id actor_id, bool apply) {
    auto* actor = find(w.objects, actor_id); if (!actor) return refused(Refusal::actor);
    actor->stealth = apply; Outcome r; r.success = true; emit(r, Effect::stealth, actor_id, actor->owner, apply ? 1U : 0U); return r;
}
} // namespace eawr::sim::galactic::hero_actions
