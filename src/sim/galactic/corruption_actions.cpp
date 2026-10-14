#include "eawr/sim/galactic/corruption_actions.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic::corruption_actions {
namespace {
template<class T> auto find(std::vector<T>& values, Id id) {
    return std::find_if(values.begin(), values.end(), [id](const auto& v) { return v.id == id; });
}
template<class T> auto find(const std::vector<T>& values, Id id) {
    return std::find_if(values.begin(), values.end(), [id](const auto& v) { return v.id == id; });
}
auto account(Fixture& f, Id id) {
    return std::find_if(f.accounts.begin(), f.accounts.end(), [id](const auto& a) { return a.player.id == id; });
}
auto account(const Fixture& f, Id id) {
    return std::find_if(f.accounts.begin(), f.accounts.end(), [id](const auto& a) { return a.player.id == id; });
}
bool valid(Type t) { return t > Type::none && t <= Type::bonded_citizens; }
bool has(std::span<const Id> ids, Id id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }
core::Diagnostic invalid(const char* message) {
    core::Diagnostic d; d.code = "EAWR-GC-CORRUPTION-ACTION-0001"; d.message = message; return d;
}
// WGCH-49/62: count the immediate transport once across all alternatives.
std::vector<const Unit*> candidates(const Fixture& f, Id owner, const Requirements& r) {
    std::vector<const Unit*> result;
    std::set<Id> seen;
    for (const auto& u : f.units) {
        if (!u.present || u.owner != owner || !has(r.unit_types, u.original_type)
            || (u.consumable && u.planet == 0)) continue;
        const Id identity = u.container ? u.container : u.id;
        if (seen.insert(identity).second) result.push_back(&u);
    }
    return result;
}
Execution refused(Refusal r) { Execution e; e.refusal = r; return e; }
} // namespace

Preparation prepare(const Fixture& f, Id actor_id, Id planet_id, bool tutorial,
    std::span<const Id> activated) {
    Preparation result;
    auto fail = [&](Refusal reason) { result.refusal = reason; return result; };
    if (!tutorial) return fail(Refusal::tutorial); // WGCH-01
    const auto actor = find(f.actors, actor_id);
    if (actor == f.actors.end() || !actor->present) return fail(Refusal::actor);
    const auto owner = account(f, actor->owner);
    if (owner == f.accounts.end()) return fail(Refusal::owner);
    const auto planet = find(f.locations, planet_id);
    if (planet == f.locations.end() || !planet->present) return fail(Refusal::planet);
    if (actor->flattened_fleet_count && *actor->flattened_fleet_count > 1) return fail(Refusal::fleet);
    for (Id name : activated) {
        if (has(actor->appropriate_abilities, name)) { result.ability = name; break; }
    }
    if (!result.ability) return fail(Refusal::ability);
    // WGCH-48 uses object owner, never model.target_allegiance.
    if (planet->owner == actor->owner || owner->allies.contains(planet->owner)) return fail(Refusal::allied_owner);
    if (planet->neutral_owner) return fail(Refusal::neutral_owner);
    if (planet->destroyed) return fail(Refusal::destroyed);
    if (planet->model.transition.active()) return fail(Refusal::transitioning);
    if (planet->model.transition.current != Type::none) return fail(Refusal::corrupted);
    if (!planet->retains_influence) return fail(Refusal::residual_influence);
    for (const auto& slot : planet->slots) { // WGCH-04, exactly authored slots 1..3
        if (!valid(slot.type)) continue;
        auto choice = slot;
        std::erase_if(choice.requirements.planet_types, [&](Id type) {
            return std::none_of(f.locations.begin(), f.locations.end(), [type](const auto& p) {
                return p.present && p.type == type;
            });
        });
        result.choices.push_back({actor_id, planet_id, std::move(choice)});
    }
    if (result.choices.empty()) return fail(Refusal::no_choices);
    return result;
}

Refusal prerequisites(const Fixture& f, const Account& a, const Choice& c) {
    if (a.player.credits < c.credits) return Refusal::credits;
    const auto& r = c.requirements;
    if (r.unit_count && !r.unit_types.empty() && candidates(f, a.player.id, r).size() < r.unit_count)
        return Refusal::units;
    if (valid(r.mission_type) && r.mission_count && a.uses[static_cast<std::size_t>(r.mission_type)] < r.mission_count)
        return Refusal::mission_usage;
    if (!r.planet_types.empty() && std::none_of(f.locations.begin(), f.locations.end(), [&](const auto& p) {
        return p.present && p.owner == a.player.id && has(r.planet_types, p.type);
    })) return Refusal::required_planet;
    return Refusal::none; // WGCH-49: alternatives aggregate, planets use ANY
}

Fixed ai_score(Type type, unsigned uses) {
    const auto reciprocal = Fixed::from_ratio(1, static_cast<std::int64_t>(uses) + 1).value();
    switch (type) { // WGCH-50 hardcoded action scores
    case Type::intimidation: return Fixed::from_ratio(2, static_cast<std::int64_t>(uses) + 1).value();
    case Type::piracy: return math::add(reciprocal, one).value();
    case Type::kidnapping: case Type::bonded_citizens: return Fixed::from_ratio(3, 4).value();
    case Type::racketeering: return Fixed::from_ratio(3, 2).value();
    case Type::militia: return math::add(reciprocal, Fixed::from_ratio(1, 2).value()).value();
    case Type::bribery: return Fixed::from_ratio(1, 4).value();
    case Type::black_market: return Fixed::from_ratio(5, 2).value();
    default: return {};
    }
}

core::Result<std::optional<Prepared>> select_ai(const Fixture& f, const Preparation& p, Fixed draw) {
    using R = core::Result<std::optional<Prepared>>;
    if (draw < Fixed{} || draw >= one) return R::failure(invalid("WGCH-03 unit draw outside [0,1)"));
    std::vector<std::pair<const Prepared*, Fixed>> options;
    Fixed total;
    if (p.refusal != Refusal::none) return R::success(std::nullopt);
    for (const auto& choice : p.choices) {
        if (!valid(choice.choice.type)) continue;
        const auto actor = find(f.actors, choice.actor);
        if (actor == f.actors.end() || !actor->present) continue;
        const auto a = account(f, actor->owner);
        if (a == f.accounts.end() || prerequisites(f, *a, choice.choice) != Refusal::none) continue;
        const auto score = ai_score(choice.choice.type, a->uses[static_cast<std::size_t>(choice.choice.type)]);
        if (score > Fixed{}) { options.emplace_back(&choice, score); total = math::add(total, score).value(); }
    }
    if (options.empty()) return R::success(std::nullopt);
    // Fixed multiply rounds; clamp a near-one draw below total for a half-open distribution.
    const auto threshold = std::min(math::multiply(total, draw).value(), Fixed::from_raw(total.raw() - 1));
    Fixed cumulative;
    for (const auto& [choice, weight] : options) {
        cumulative = math::add(cumulative, weight).value();
        if (threshold < cumulative) return R::success(*choice);
    }
    return R::success(std::nullopt);
}

core::Result<Execution> execute(Fixture& f, const Prepared& p, const ExecutionInput& input) {
    using R = core::Result<Execution>;
    if (!valid(p.choice.type)) return R::failure(invalid("invalid prepared corruption type"));
    auto actor = find(f.actors, p.actor);
    if (actor == f.actors.end() || !actor->present) return R::success(refused(Refusal::actor));
    auto a = account(f, actor->owner);
    if (a == f.accounts.end()) return R::success(refused(Refusal::owner));
    auto planet = find(f.locations, p.planet);
    if (planet == f.locations.end() || !planet->present) return R::success(refused(Refusal::planet));
    if (planet->model.transition.active()) return R::success(refused(Refusal::transitioning));
    if (planet->model.transition.current != Type::none) return R::success(refused(Refusal::corrupted));
    if (a->player.credits < p.choice.credits) return R::success(refused(Refusal::credits));
    Execution result;
    // WGCH-05/72: do not repeat mission, required-planet, owner or tutorial predicates.
    if (p.choice.credits > Fixed{}) {
        auto balance = math::subtract(a->player.credits, p.choice.credits);
        if (!balance) return R::failure(balance.error());
        a->player.credits = balance.value();
        result.events.push_back({Effect::debit, p.actor, a->player.id});
    }
    const auto& requirements = p.choice.requirements;
    auto selected = candidates(f, a->player.id, requirements);
    if (requirements.unit_count && !requirements.unit_types.empty() && selected.size() < requirements.unit_count) {
        result.refusal = Refusal::units; return R::success(std::move(result));
    }
    // WGCH-06/07/62. Named prerequisites use default hero scheduling; generic
    // candidates use descending synchronized weights. Equal-weight order is U-03.
    std::sort(selected.begin(), selected.end(), [](const Unit* l, const Unit* r) {
        return l->weight != r->weight ? l->weight > r->weight : l->tie_rank < r->tie_rank;
    });
    const auto count = std::min(selected.size(), static_cast<std::size_t>(requirements.unit_count));
    // Validate the host's unresolved ordering seam before consuming any units,
    // including a tied runner-up when only one prerequisite will be consumed.
    if (count) for (std::size_t i = 0; i < selected.size(); ++i) {
        const auto& u = *selected[i];
        if (!u.consumable || u.named) continue;
        if (u.weight > 4) return R::failure(invalid("WGCH-07 prerequisite weight outside [0,4]"));
    }
    for (std::size_t i = 1; i <= count && i < selected.size(); ++i) {
        const auto& u = *selected[i];
        if (selected[i - 1]->weight == u.weight && selected[i - 1]->tie_rank == u.tie_rank)
            return R::failure(invalid("U-03 unresolved prerequisite tie order"));
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& u = *selected[i];
        if (!u.consumable) continue;
        // WGCH-06 removes the named instance; WGCH-07 destroys generic
        // breakdown/container contents. Do not consume a named hero's siblings.
        const Id identity = u.named ? u.id : (u.container ? u.container : u.id);
        if (u.named) result.events.push_back({Effect::schedule_prerequisite_default, u.id, a->player.id});
        result.events.push_back({Effect::destroy_prerequisite, identity, a->player.id});
        for (auto& member : f.units) {
            if (member.id == identity || (!u.named && member.container == identity)) member.present = false;
        }
    }
    const bool tactical = p.choice.map && (p.choice.mode == MissionMode::land || p.choice.mode == MissionMode::space)
        && !a->ai && !input.multiplayer && !input.fast_corruption; // WGCH-08
    if (tactical) {
        result.events.push_back({Effect::queue_mission, p.planet, a->player.id});
        if (!input.mission_queue_accepts) { result.refusal = Refusal::mission_queue; return R::success(std::move(result)); }
        result.mission = Mission{p.actor, p.planet, a->player.id, p.choice.map, p.choice.script, p.choice.type, p.choice.mode};
        result.mission->local_success_permitted = input.local_success_permitted;
    } else {
        auto& uses = a->uses[static_cast<std::size_t>(p.choice.type)];
        if (uses == std::numeric_limits<unsigned>::max()) return R::failure(invalid("corruption usage overflow"));
        auto started = begin(planet->model.transition, p.choice.type, Fixed::from_integer(5).value(), input.logical_fps, input.now);
        if (!started) return R::failure(started.error());
        if (!started.value()) { result.refusal = Refusal::transition; return R::success(std::move(result)); }
        planet->model.corrupting_player = a->player.id;
        result.events.push_back({Effect::begin_transition, p.planet, a->player.id});
        ++uses;
        result.events.push_back({Effect::increment_usage, p.planet, a->player.id});
        result.events.push_back({Effect::hero_arrival, p.actor, a->player.id});
        if (a->local && input.local_success_permitted) result.events.push_back({Effect::local_success, p.actor, a->player.id});
    }
    result.events.push_back({Effect::schedule_actor_default, p.actor, a->player.id});
    result.events.push_back({Effect::remove_actor, p.actor, a->player.id});
    actor->present = false;
    return R::success(std::move(result));
}

core::Result<Execution> finish_mission(Fixture& f, Mission& mission, MissionState outcome,
    std::uint32_t fps, Frame now) {
    using R = core::Result<Execution>;
    if (mission.state != MissionState::queued || outcome == MissionState::queued)
        return R::success(refused(Refusal::mission_state));
    if (outcome > MissionState::cancelled || !valid(mission.type))
        return R::failure(invalid("invalid mission outcome/type"));
    Execution result;
    if (outcome == MissionState::failed || outcome == MissionState::cancelled) {
        mission.state = outcome;
        if (outcome == MissionState::failed) result.events.push_back({Effect::mission_failure, mission.planet, mission.player});
        return R::success(std::move(result));
    }
    auto planet = find(f.locations, mission.planet);
    if (planet == f.locations.end() || !planet->present) return R::success(refused(Refusal::planet));
    auto a = account(f, mission.player);
    if (a == f.accounts.end()) return R::success(refused(Refusal::owner));
    auto& uses = a->uses[static_cast<std::size_t>(mission.type)];
    if (uses == std::numeric_limits<unsigned>::max()) return R::failure(invalid("corruption usage overflow"));
    auto started = begin(planet->model.transition, mission.type, Fixed::from_integer(5).value(), fps, now);
    if (!started) return R::failure(started.error());
    if (!started.value()) return R::success(refused(Refusal::transition));
    planet->model.corrupting_player = mission.player;
    mission.state = outcome;
    result.events.push_back({Effect::begin_transition, mission.planet, mission.player});
    ++uses; // WGCH-10/WGM-81: result usage/speech/story, not normal conquest.
    result.events.push_back({Effect::increment_usage, mission.planet, mission.player});
    if (a->local && mission.local_success_permitted)
        result.events.push_back({Effect::local_success, mission.actor, mission.player});
    result.events.push_back({Effect::mission_success, mission.planet, mission.player});
    return R::success(std::move(result));
}
} // namespace eawr::sim::galactic::corruption_actions
