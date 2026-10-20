#include "eawr/sim/galactic/control.hpp"

#include "encoding.hpp"
#include "session_impl.hpp"
#include "native_control_admission.hpp"
#include "../replay_internal.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <map>
#include <limits>
#include <set>
#include <tuple>

namespace eawr::sim::galactic {
namespace {
using math::Fixed;
constexpr Fixed one = Fixed::from_raw(Fixed::scale);
constexpr Fixed five = Fixed::from_raw(5 * Fixed::scale);
template<class T> core::Result<T> invalid(const std::string& message) {
    return core::Result<T>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, message));
}
template<class T, class Key> const T* find_row(const std::vector<T>& rows, const auto key, Key get) {
    const auto it = std::lower_bound(rows.begin(), rows.end(), key,
        [&](const T& row, const auto value) { return get(row) < value; });
    return it != rows.end() && get(*it) == key ? &*it : nullptr;
}
std::vector<std::uint8_t> built_structure_receipt(bool add, PlanetId planet,
    const TacticalBuiltStructure& record, PlayerId holder) {
    std::vector<std::uint8_t> bytes;
    detail::put_u8(bytes, add ? 1 : 2); detail::put_u64(bytes, planet); detail::put_u64(bytes, record.type);
    detail::put_i64(bytes, record.position.x.raw()); detail::put_i64(bytes, record.position.y.raw()); detail::put_i64(bytes, record.position.z.raw());
    detail::put_u32(bytes, record.enable_level); detail::put_u32(bytes, record.owner); detail::put_u32(bytes, holder);
    return bytes;
}
void fix_built_structures(std::vector<TacticalBuiltStructure>& records, PlayerId winner) {
    // WGM-187: remove backwards; winning values and build order remain intact.
    for (std::size_t i = records.size(); i > 0; --i)
        if (records[i - 1].owner != winner) records.erase(records.begin() + static_cast<std::ptrdiff_t>(i - 1));
}
bool allied(const PoliticalControlSetup& s, PlayerId a, PlayerId b) {
    const auto* relation = find_row(s.relations, std::pair{a, b},
        [](const auto& row) { return std::pair{row.player, row.other}; });
    return relation && relation->allied;
}
const ControlType* type(const PoliticalControlSetup& s, TypeId id) {
    return find_row(s.types, id, [](const auto& row) { return row.type; });
}
std::uint32_t landed_count(const ControlPlanet& surface, const std::map<EntityId, CensusObject>& actors) {
    return session_detail::native_landed_count(surface, actors);
}

std::vector<FleetMember> flattened(std::span<const FleetMember> members, const MovementSetup* movement) {
    std::vector<FleetMember> pending(members.rbegin(), members.rend()), result;
    while (!pending.empty()) {
        const auto member = pending.back(); pending.pop_back();
        const auto* contents = movement ? find_row(movement->contents, member.id,
            [](const auto& row) { return row.container; }) : nullptr;
        if (contents) pending.insert(pending.end(), contents->children.rbegin(), contents->children.rend());
        else result.push_back(member);
    }
    return result;
}
bool skips(const PoliticalControlSetup& s, const std::vector<FleetMember>& members) {
    // WGM-38 / WGM-C03: stealth is the same live combat-skip predicate here.
    return !members.empty() && std::all_of(members.begin(), members.end(),
        [&](const auto& member) { const auto* t = type(s, member.type); return t && t->stealth; });
}
std::vector<FleetMember> live_breakdown(const PoliticalControlSetup& s,
    const std::span<const FleetMember> members, const MovementSetup* movement) {
    auto rows = flattened(members, movement);
    std::erase_if(rows, [&](const auto& row) { return std::binary_search(s.absent_forces.begin(), s.absent_forces.end(), row.id); });
    return rows;
}
template<class T, class Key> bool ordered(const std::vector<T>& rows, Key get) {
    for (std::size_t i = 1; i < rows.size(); ++i) if (!(get(rows[i - 1]) < get(rows[i]))) return false;
    return true;
}
} // namespace

core::Result<bool> has_ground_capability(const PoliticalControlSetup& inputs,
    FleetMember object, const MovementSetup* movement) {
    if (!object.id) return core::Result<bool>::success(false);
    if (inputs.ground_capability_types.empty()) {
        const auto* legacy = type(inputs, object.type);
        if (!legacy) return invalid<bool>("ground capability requires a bound current type");
        return core::Result<bool>::success(legacy->ground_capable);
    }
    const auto fact = [&](TypeId id) {
        return find_row(inputs.ground_capability_types, id, [](const auto& row) { return row.type; });
    };
    const auto* current = fact(object.type);
    if (!current) return invalid<bool>("ground capability requires sourced current-type facts");
    if (!current->transport || current->stays_in_transport) return core::Result<bool>::success(false);
    const auto* contents = movement ? find_row(movement->contents, object.id,
        [](const auto& row) { return row.container; }) : nullptr;
    if (!contents) return core::Result<bool>::success(false);
    for (const auto& payload : contents->children) {
        if (!payload.id) continue;
        const auto* source = fact(payload.type);
        if (!source) return invalid<bool>("ground capability requires sourced direct-payload facts");
        if (source->combat_power.raw() > 0 && !source->stays_in_transport)
            return core::Result<bool>::success(true);
    }
    return core::Result<bool>::success(false);
}

core::Result<void> validate_control_setup(const GalacticSetup& setup) {
    if (!setup.control) return core::Result<void>::success();
    const auto& s = *setup.control;
    if (s.change_seconds.raw() <= 0 || s.max_ground_forces == 0 || s.max_ground_forces > galactic_max_objects
        || s.players.size() != setup.players.size() || s.planets.size() != setup.planets.size()
        || s.routes.size() != setup.routes.size() || (!s.state.empty() && s.state.size() != s.planets.size())
        || s.types.size() > galactic_max_objects || s.captured_types.size() > galactic_max_objects)
        return invalid<void>("invalid political control counts or authored constants");
    if (!ordered(s.types, [](const auto& r) { return r.type; })
        || !ordered(s.relations, [](const auto& r) { return std::pair{r.player, r.other}; })
        || !ordered(s.captured_types, [](const auto& r) { return std::pair{r.player, r.type}; }))
        return invalid<void>("political control tables must be in canonical order");
    if (s.absent_forces.size() > galactic_max_objects || !ordered(s.absent_forces, [](auto id) { return id; })
        || std::any_of(s.absent_forces.begin(), s.absent_forces.end(), [](auto id) { return id == 0; }))
        return invalid<void>("invalid absent control roster");
    const auto known = [&](PlayerId id) {
        return find_row(setup.players, id, [](const auto& r) { return r.player_id; }) != nullptr;
    };
    std::size_t neutrals = 0;
    for (std::size_t i = 0; i < s.players.size(); ++i) {
        if (s.players[i].player != setup.players[i].player_id || s.players[i].faction != setup.players[i].faction_id
            || (s.players[i].neutral && s.players[i].playable))
            return invalid<void>("political control player table differs from galaxy");
        if (s.players[i].neutral) ++neutrals;
    }
    if (neutrals != 1 || s.relations.size() != s.players.size() * s.players.size())
        return invalid<void>("control requires one neutral player and complete relation inputs");
    for (const auto& r : s.relations) if (!known(r.player) || !known(r.other) || (r.player == r.other && !r.allied))
        return invalid<void>("invalid political relation");
    for (const auto& t : s.types) if (t.type == 0 || t.political_control > 1)
        return invalid<void>("orbital political control must be zero or one");
    if (!s.ground_capability_types.empty()) {
        if (s.ground_capability_types.size() != s.types.size()) return invalid<void>("ground capability facts must cover every control type");
        for (std::size_t i = 0; i < s.types.size(); ++i) {
            const auto& fact = s.ground_capability_types[i];
            if (fact.type != s.types[i].type || fact.combat_power.raw() < -(std::int64_t{1} << 40)
                || fact.combat_power.raw() > (std::int64_t{1} << 40)) return invalid<void>("invalid ground capability current-type facts");
        }
    }
    std::set<EntityId> identities;
    for (const auto& p : setup.planets) identities.insert(p.id);
    for (const auto& r : setup.routes) identities.insert(r.id);
    for (const auto& f : setup.fleets) {
        identities.insert(f.id);
        for (const auto& m : f.members) identities.insert(m.id);
        for (const auto& m : flattened(f.members, setup.movement ? &*setup.movement : nullptr))
            if (!type(s, m.type)) return invalid<void>("missing live orbital political type");
    }
    if (setup.fleet_control) for (const auto& u : setup.fleet_control->detached_units) {
        identities.insert(u.member.id);
        for (const auto& m : flattened(std::span(&u.member, 1), setup.movement ? &*setup.movement : nullptr))
            if (!type(s, m.type)) return invalid<void>("missing detached political type for future attachment");
    }
    if (setup.movement) for (const auto& c : setup.movement->contents) for (const auto& m : c.children) identities.insert(m.id);
    std::size_t nested_count = identities.size();
    Fixed denominator{}, delta{};
    if (!math::try_multiply(s.change_seconds, Fixed::from_raw(30 * Fixed::scale), denominator)
        || !math::try_divide(one, denominator, delta) || delta.raw() <= 0)
        return invalid<void>("political transition duration is outside fixed-point service range");
    for (std::size_t i = 0; i < s.planets.size(); ++i) {
        const auto& p = s.planets[i];
        if (p.planet != setup.planets[i].id || p.type == 0 || p.leaning.raw() < 0 || p.capture_reward.raw() < 0
            || p.special_control.size() > 3
            || (p.leaning.raw() > 0 && !known(p.leaning_player))
            || (p.ground_base.level && !known(p.ground_base.owner))
            || (p.starbase.level && !known(p.starbase.owner))
            || p.ground_base.level > 5 || p.starbase.level > 5
            || !ordered(p.caps, [](const auto& r) { return r.player; })
            || !ordered(p.transition_modifiers, [](const auto& r) { return std::pair{r.category, r.source}; }))
            return invalid<void>("invalid planet political presence or modifier input");
        for (const auto owner : p.production_owner) if (owner && !known(owner)) return invalid<void>("unknown production controller");
        for (const auto& cap : p.caps) if (!known(cap.player) || cap.maximum > 5) return invalid<void>("invalid political cap");
        for (const auto& modifier : p.transition_modifiers)
            if (modifier.source == 0 || modifier.multiplier.raw() <= 0) return invalid<void>("invalid transition modifier");
        std::size_t live_landed{};
        // WGS-181: authored surface transports retain their individual owners.
        for (const auto& force : p.landed) {
            if (!known(force.owner) || !type(s, force.type) || force.id == 0 || !identities.insert(force.id).second
                || force.id == std::numeric_limits<EntityId>::max()
                || std::binary_search(s.absent_forces.begin(), s.absent_forces.end(), force.id))
                return invalid<void>("surface forces must be live unique identities with valid owners");
            // WGA-410: archived delete-pending entries consume no live slot.
            const auto* actors = setup.census ? &setup.census->objects : nullptr;
            const auto actual = actors ? std::find_if(actors->begin(), actors->end(),
                [&](const auto& actor) { return actor.id == force.id; }) : std::vector<CensusObject>::const_iterator{};
            if (!actors || actual == actors->end() || !actual->delete_pending) ++live_landed;
        }
        if (live_landed > s.max_ground_forces) return invalid<void>("surface live force count exceeds configured capacity");
        for (const auto& record : p.built_structures)
            if (!record.type || !known(record.owner) || record.enable_level > 5)
                return invalid<void>("invalid tactical built-object record (WGT-84)");
        nested_count += p.built_structures.size() + p.landed.size() + p.special_control.size() + p.caps.size() + p.transition_modifiers.size();
        if (!s.state.empty()) {
            const auto& state = s.state[i];
            if (state.planet != p.planet || !known(state.current_player) || (state.target_player && !known(state.target_player))
                || (state.controller && !known(state.controller)) || (state.orbital_player && !known(state.orbital_player))
                || state.current < Fixed{} || state.current > five || state.target < Fixed{} || state.target > five
                || (state.initialized && state.current_player != setup.planets[i].owner))
                return invalid<void>("invalid current or target political state");
        }
    }
    if (nested_count > galactic_max_objects) return invalid<void>("political inputs exceed object budget");
    for (std::size_t i = 0; i < s.routes.size(); ++i) if (s.routes[i].route != setup.routes[i].id)
        return invalid<void>("political routes differ from galaxy");
    for (const auto& row : s.captured_types) if (!known(row.player) || row.type == 0)
        return invalid<void>("invalid captured planet type history");
    return core::Result<void>::success();
}

core::Result<ControlStep> calculate_planet_control(const PoliticalControlSetup& s, const ControlPlanet& p,
    const PlanetControl& previous, const std::span<const Fleet> orbit, const MovementSetup* movement,
    const std::span<const Route> routes, const std::uint64_t frame, const std::span<const CensusObject> actors,
    const std::map<FleetId, core::Result<bool>>* cached_skip) {
    ControlStep out; out.state = previous;
    auto& state = out.state;
    PlayerId controller = p.ground_base.level ? p.ground_base.owner : 0;
    Fixed military = Fixed::from_raw(static_cast<std::int64_t>(p.ground_base.level) * Fixed::scale);
    // WGS-186 / WGM-66: political control skips deleting transports, unlike
    // the ground-occupation query, which reads the first linked entry.
    const auto first_live = std::find_if(p.landed.begin(), p.landed.end(), [&](const auto& force) {
        const auto actor = std::lower_bound(actors.begin(), actors.end(), force.id,
            [](const auto& row, auto id) { return row.id < id; });
        return actor == actors.end() || actor->id != force.id || !actor->delete_pending;
    });
    if (first_live != p.landed.end()) { military = std::max(military, one); if (!controller) controller = first_live->owner; }
    military = std::max(military, Fixed::from_raw(static_cast<std::int64_t>(p.starbase.level) * Fixed::scale));
    if (!controller && p.starbase.level) controller = p.starbase.owner;
    if (!controller) for (auto owner : p.production_owner) if (owner) { controller = owner; break; }
    PlayerId orbital = p.starbase.level ? p.starbase.owner : 0, stealth_fallback = 0;
    std::int64_t fleet_control = 0;
    PlayerId blockade_owner = 0;
    std::vector<PlayerId> ordinary_owners;
    // WGM-67 and WGM-C01/C02: caller supplies runtime orbit-list order.
    for (const auto& fleet : orbit) {
        const auto members = live_breakdown(s, fleet.members, movement);
        if (members.empty()) continue;
        const auto live_skip = cached_skip ? (cached_skip->contains(fleet.id) ? cached_skip->at(fleet.id)
            : invalid<bool>("political_live_fleet_skip_unqualified"))
            : actors.empty() ? core::Result<bool>::success(skips(s, members))
            : fleet_combat_skip(fleet, actors, movement);
        if (!live_skip) return core::Result<ControlStep>::failure(live_skip.error());
        const bool stealth = live_skip.value();
        if (!orbital) {
            if (!stealth) orbital = fleet.owner;
            else if (!stealth_fallback) stealth_fallback = fleet.owner;
        }
        if (!stealth) { if (!blockade_owner) blockade_owner = fleet.owner; ordinary_owners.push_back(fleet.owner); }
        if (!controller && !p.surface_accessible && !stealth) { controller = fleet.owner; military = std::max(military, one); }
        for (const auto& member : members) {
            ++out.member_visits;
            const auto* t = type(s, member.type);
            if (!t) return invalid<ControlStep>("missing live political type at control service");
            fleet_control += fleet.owner == controller ? t->political_control : -static_cast<std::int64_t>(t->political_control);
        }
    }
    if (!orbital) orbital = stealth_fallback;
    fleet_control = std::clamp(fleet_control, std::int64_t{-1}, std::int64_t{1});
    if (fleet_control >= 0) military = std::max(military, Fixed::from_raw(fleet_control * Fixed::scale));
    if (!controller && !p.destroyed && p.retains_residual) { controller = previous.current_player; military = std::max(military, one); }
    // WGM-68: caps depend on military/production/residual controller before leaning.
    const auto cap_controller = controller;
    Fixed leaning = std::min(p.leaning, five);
    if (!controller && leaning.raw() > 0) controller = p.leaning_player;
    else if (controller && leaning.raw() > 0) {
        const auto* controlling = find_row(s.players, controller, [](const auto& row) { return row.player; });
        const auto* leaning_owner = find_row(s.players, p.leaning_player, [](const auto& row) { return row.player; });
        if (!controlling || !leaning_owner) return invalid<ControlStep>("missing faction identity for planet leaning");
        if (controlling->faction != leaning_owner->faction) leaning = Fixed::from_raw(-leaning.raw());
    }
    if (!controller) for (const auto& player : s.players) if (player.neutral) { controller = player.player; break; }
    Fixed target{};
    if (!math::try_add(military, Fixed::from_raw(std::min(fleet_control, std::int64_t{0}) * Fixed::scale), target)
        || !math::try_add(target, leaning, target)) return invalid<ControlStep>("political category sum overflow");
    for (const auto value : p.special_control) if (!math::try_add(target, value, target))
        return invalid<ControlStep>("special political category overflow");
    Fixed trade{};
    for (const auto& route : routes) {
        ++out.route_visits;
        PlanetId other = route.from == p.planet ? route.to : (route.to == p.planet ? route.from : 0);
        if (!other) continue;
        const auto* other_state = find_row(s.state, other, [](const auto& row) { return row.planet; });
        const auto* input = find_row(s.routes, route.id, [](const auto& row) { return row.route; });
        if (other_state && input && previous.current >= one && other_state->current >= one
            && allied(s, other_state->current_player, controller)
            && !math::try_add(trade, input->political_gain, trade)) return invalid<ControlStep>("route political gain overflow");
    }
    if (!math::try_add(target, trade, target)) return invalid<ControlStep>("political total overflow");
    Fixed maximum = five;
    if (cap_controller) if (const auto* cap = find_row(p.caps, cap_controller, [](const auto& row) { return row.player; }))
        maximum = cap->maximum == 0 ? five : Fixed::from_raw(static_cast<std::int64_t>(cap->maximum) * Fixed::scale);
    target = std::clamp(target, cap_controller ? one : Fixed{}, maximum);
    target = std::clamp(target, Fixed{}, five);
    state.controller = cap_controller; state.orbital_player = orbital;
    state.contested = std::any_of(ordinary_owners.begin(), ordinary_owners.end(), [&](auto owner) { return !allied(s, owner, controller); });
    if (!state.initialized) {
        state.initialized = true; state.current = target; state.current_player = controller;
        state.target = target; state.target_player = controller;
    }
    const auto before = state;
    const bool degrading = controller != state.current_player || target < state.current;
    if (state.target_player != controller || state.target != target) {
        const auto prior_target = state.target_player;
        state.target_player = controller; state.target = target;
        // WGM-72: a changed target player resets CURRENT player and amount before protection/service.
        if (controller != prior_target) { state.current_player = controller; state.current = Fixed{}; }
    }
    if (!degrading || !p.protected_control) {
        std::map<std::uint32_t, Fixed> modifiers;
        for (const auto& row : p.transition_modifiers) {
            // WGM-C06: a deadline remains active at equality; expiry is strict.
            if (row.expires_frame && frame > row.expires_frame) continue;
            const auto it = modifiers.find(row.category);
            if (it == modifiers.end()) modifiers.emplace(row.category, row.multiplier);
            else it->second = std::min(it->second, row.multiplier);
        }
        Fixed multiplier = one, denominator{}, delta{};
        for (const auto& [category, value] : modifiers) {
            (void)category;
            if (!math::try_multiply(multiplier, value, multiplier)) return invalid<ControlStep>("transition multiplier overflow");
        }
        if (!math::try_multiply(s.change_seconds, multiplier, denominator)
            || !math::try_multiply(denominator, Fixed::from_raw(30 * Fixed::scale), denominator)
            || !math::try_divide(one, denominator, delta) || delta.raw() <= 0)
            return invalid<ControlStep>("invalid political transition service rate");
        if (state.current_player == state.target_player) {
            if (state.current < target) state.current = Fixed::from_raw(state.current.raw() + std::min(delta.raw(), target.raw() - state.current.raw()));
            else state.current = Fixed::from_raw(state.current.raw() - std::min(delta.raw(), state.current.raw() - target.raw()));
        } else {
            state.current = Fixed::from_raw(state.current.raw() - std::min(delta.raw(), state.current.raw()));
            if (state.current == Fixed{}) state.current_player = state.target_player;
        }
    }
    out.owner_changed = before.current_player != state.current_player;
    out.stop_production = before.current >= one && state.current < one;
    state.blockaded = blockade_owner && !allied(s, blockade_owner, state.current_player);
    return core::Result<ControlStep>::success(std::move(out));
}

// WGM-53/60/188: one structure actor leaves its planet: the census row, the owner's counts and population, and the
// original-type record. The caller erases the production row. Shared by clearing and live sabotage (WGCH-29).
void GalacticSession::Impl::release_structure_object(PlanetProduction& planet, const ProductionObject& object) {
    lose_actor(object.id);
    const auto owner = std::lower_bound(production->players.begin(), production->players.end(), object.owner,
        [](const auto& row, auto id) { return row.player < id; });
    if (owner != production->players.end() && owner->player == object.owner) {
        const auto count = std::lower_bound(owner->counts.begin(), owner->counts.end(), object.type,
            [](const auto& row, auto id) { return row.type < id; });
        if (count != owner->counts.end() && count->type == object.type && count->owned) --count->owned;
        const auto offer = find_row(planet.offers, object.type, [](const auto& row) { return row.type; });
        if (offer) owner->population_used = std::max(Fixed{}, math::subtract(owner->population_used, offer->population).value());
    }
    if (std::erase_if(production_originals, [&](const auto& row) { return row.id == object.id; }))
        published_production_originals.reset();
    --live_object_count;
}

void GalacticSession::Impl::clear_control_structures(const std::size_t index, const std::optional<ProductionDomain> only,
    std::vector<EntityId>* const removed_ids) {
    if (!production) return;
    auto& planet = production->planets[index];
    // WGM-53/60/188: remove actual ground/orbital special actors, not just
    // political modifier values. This is an invasion/return commit, never a tick scan.
    bool removed = false;
    std::erase_if(planet.objects, [&](const auto& object) {
        if (object.kind != ProductionKind::structure || (only && object.domain != *only)) return false;
        if (removed_ids) removed_ids->push_back(object.id);
        release_structure_object(planet, object);
        removed = true;
        return true;
    });
    if (!removed) return;
    dirty[static_cast<std::size_t>(StateSection::production)] = true;
    population_stale = true;
    invalidate_observer_projection(); // also invalidates economic source gathers
}

void GalacticSession::Impl::update_production_control(const std::size_t index, const std::uint64_t tick,
    std::vector<GalacticEvent>& events, const bool stop) {
    if (!production) return;
    auto& row = production->planets[index];
    const auto& state = control->state[index]; const auto& presence = control->planets[index];
    bool changed = false;
    const auto set = [&](auto& target, const auto value) { if (target != value) { target = value; changed = true; } };
    set(row.target_owner, state.target_player);
    set(row.enabled, setup.production->planets[index].enabled && state.current >= one);
    set(row.ground_occupier, presence.ground_base.level ? presence.ground_base.owner
        : (presence.landed.empty() ? PlayerId{0} : presence.landed.front().owner));
    set(row.orbit_occupier, state.orbital_player);
    set(row.landed_transports, static_cast<std::int64_t>(presence.landed.size()));
    set(row.ground_level, static_cast<std::int64_t>(presence.ground_base.level));
    set(row.starbase_owner, presence.starbase.owner);
    set(row.space_level, static_cast<std::int64_t>(presence.starbase.level));
    if (!presence.starbase.level) set(row.starbase_type, TypeId{0});
    if (stop) {
        // WGM-74 / WGEP-56: forward cancellation of both queues, no direct refund.
        for (auto& queue : row.queues) {
            for (const auto& entry : queue) {
                events.push_back({tick, GalacticEventKind::production_cancelled, {}, row.planet,
                    entry.build_id, entry.type, GalacticRejectReason::none});
                if (native_production_outcome_handler) native_production_outcome_handler({entry.builder, entry.build_id, 0, false});
            }
            if (!queue.empty()) changed = true;
            queue.clear();
        }
        active_producers.erase(index);
    }
    if (changed) dirty[static_cast<std::size_t>(StateSection::production)] = true;
}

core::Result<void> GalacticSession::Impl::service_control(const PartitionExecutor& executor,
    const std::uint64_t tick, std::vector<GalacticEvent>& events, GalacticTick::Work& work) {
    if (!control) return core::Result<void>::success();
    const auto inputs = *control; // immutable same-frame control/trade inputs
    const auto route_rows = sorted_routes();
    const auto fleet_rows = sorted_fleets();
    std::map<PlanetId, std::vector<Route>> incident_routes;
    for (const auto& route : route_rows) { incident_routes[route.from].push_back(route); incident_routes[route.to].push_back(route); }
    std::map<PlanetId, std::vector<Fleet>> orbit;
    for (const auto& fleet : fleet_rows) if (fleet.orbit) orbit[fleet.orbit].push_back(fleet);
    for (auto& [id, rows] : orbit) {
        (void)id;
        std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.motion.orbit_entry < b.motion.orbit_entry; });
    }
    std::vector<std::optional<core::Result<ControlStep>>> staged(inputs.planets.size());
    const auto actors = actor_inputs(); // immutable publication, rebuilt only on writes
    // GFS-07 services control every eligible frame. Recursive actor ability
    // reads belong to a dirty input phase, not each planet service invocation.
    if (!actors.empty() && control_stealth_generation != observer_input_generation) {
        std::vector<std::optional<core::Result<bool>>> skip_rows(fleet_rows.size());
        const auto prepared = executor.execute_phase("galactic-control-stealth-inputs", tick_partition_count,
            [&](std::size_t partition) {
                const auto range = partition_range(partition, fleet_rows.size());
                for (auto i = range.begin; i < range.end; ++i)
                    skip_rows[i] = fleet_combat_skip(fleet_rows[i], actors, movement_inputs.get());
            });
        if (!prepared) return core::Result<void>::failure(prepared.error());
        control_stealth_inputs.clear();
        for (std::size_t i = 0; i < fleet_rows.size(); ++i)
            control_stealth_inputs.emplace(fleet_rows[i].id, std::move(*skip_rows[i]));
        control_stealth_generation = observer_input_generation;
        work.control_stealth_fleets += fleet_rows.size();
    }
    const auto* cached_skip = actors.empty() ? nullptr : &std::as_const(control_stealth_inputs);
    const auto phase = executor.execute_phase("galactic-control", tick_partition_count, [&](std::size_t partition) {
        const auto range = partition_range(partition, inputs.planets.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto it = std::as_const(orbit).find(inputs.planets[i].planet);
            const std::span<const Fleet> presence = it == orbit.end() ? std::span<const Fleet>{} : it->second;
            const auto links = std::as_const(incident_routes).find(inputs.planets[i].planet);
            const std::span<const Route> routes = links == incident_routes.end() ? std::span<const Route>{} : links->second;
            staged[i] = calculate_planet_control(inputs, inputs.planets[i], inputs.state[i], presence,
                movement_inputs ? movement_inputs.get() : nullptr, routes, clock.galactic_frame, actors, cached_skip);
        }
    });
    if (!phase) return core::Result<void>::failure(phase.error());
    for (const auto& row : staged) if (!*row) return core::Result<void>::failure(row->error());
    for (std::size_t i = 0; i < staged.size(); ++i) {
        const auto& value = staged[i]->value();
        ++work.control_planets; work.control_route_visits += value.route_visits; work.control_member_visits += value.member_visits;
        const auto& before = control->state[i];
        const auto& p = control->planets[i];
        PoliticalControlEffect effect; effect.planet = p.planet;
        effect.old_owner = before.current_player; effect.new_owner = value.state.current_player;
        effect.owner_changed = value.owner_changed;
        effect.stop_production = value.stop_production;
        effect.refresh_visibility = !before.initialized || before.target_player != value.state.target_player || effect.owner_changed;
        if (before.current != value.state.current || before.current_player != value.state.current_player
            || before.blockaded != value.state.blockaded) ++income_control_generation;
        if (before != value.state) { control->state[i] = value.state; dirty[9] = true; }
        auto& planet = registry.get<session_detail::PlanetData>(planets.at(p.planet));
        if (effect.owner_changed) {
            --owned_planet_counts[planet.owner];
            ++owned_planet_counts[value.state.current_player];
            record_planet_change(p.planet, planet.owner, value.state.current_player); // WGP-101
            planet.owner = value.state.current_player; dirty[static_cast<std::size_t>(StateSection::planets)] = true;
            invalidate_income_inputs();
            relocate_actor(p.planet, planet.owner, p.planet, 0);
            // GFS-08: ownership commit tests victory before cap/conquest consumers.
            const std::array kinds{VictoryKind::all_planets, VictoryKind::no_enemy_planets,
                VictoryKind::control_fraction, VictoryKind::homeworld_and_leader, VictoryKind::named_planets};
            if (auto tested = test_conditions(executor, kinds, tick, events); !tested) return tested;
            refresh_credit_caps(); // GFD-05: clamp before conquest listeners and fiscal.
            events.push_back({tick, GalacticEventKind::planet_owner_changed, {}, p.planet, 0, planet.owner, GalacticRejectReason::none});
            const auto* player = find_row(control->players, planet.owner, [](const auto& row) { return row.player; });
            const auto key = std::pair{planet.owner, p.type};
            const auto captured = std::lower_bound(control->captured_types.begin(), control->captured_types.end(), key,
                [](const auto& row, const auto k) { return std::pair{row.player, row.type} < k; });
            if (!p.destroyed && player && player->playable
                && (captured == control->captured_types.end() || std::pair{captured->player, captured->type} != key)) {
                // WGM-76: record BEFORE reward delivery; never reward a recapture twice.
                control->captured_types.insert(captured, {planet.owner, p.type});
                effect.first_capture = true; effect.capture_reward = p.capture_reward; dirty[9] = true;
            }
        }
        sync_observer_control(i);
        if (effect.owner_changed) transfer_planet_spies(p.planet, effect.old_owner, effect.new_owner);
        update_production_control(i, tick, events, effect.stop_production);
        if (effect.owner_changed) {
            auto corrupted = capture_corruption(p.planet, effect.old_owner, effect.new_owner);
            if (!corrupted) return corrupted;
            if (auto refreshed = refresh_planet_technology_price(p.planet, true); !refreshed) return refreshed;
        }
        if (effect.owner_changed || effect.stop_production || effect.refresh_visibility) control_effects.push_back(effect);
    }
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::Impl::initialize_control() {
    if (!control || !setup.control->state.empty()) return core::Result<void>::success();
    // WGM-C04: tick-zero initialization sets current AND target without trade routes.
    // This is construction, outside the partitioned tick service.
    const auto inputs = *control;
    const auto all_fleets = sorted_fleets();
    for (std::size_t i = 0; i < inputs.planets.size(); ++i) {
        std::vector<Fleet> orbit;
        for (const auto& fleet : all_fleets) if (fleet.orbit == inputs.planets[i].planet) orbit.push_back(fleet);
        std::stable_sort(orbit.begin(), orbit.end(), [](const auto& a, const auto& b) { return a.motion.orbit_entry < b.motion.orbit_entry; });
        auto calculated = calculate_planet_control(inputs, inputs.planets[i], inputs.state[i], orbit,
            setup.movement ? &*setup.movement : nullptr, {}, 0, actor_inputs());
        if (!calculated) return core::Result<void>::failure(calculated.error());
        control->state[i] = calculated.value().state;
        registry.get<session_detail::PlanetData>(planets.at(inputs.planets[i].planet)).owner = calculated.value().state.current_player;
        relocate_actor(inputs.planets[i].planet, calculated.value().state.current_player, inputs.planets[i].planet, 0);
    }
    return core::Result<void>::success();
}

GalacticRejectReason GalacticSession::Impl::validate_landing(PlayerId player, FleetId id, PlanetId planet, bool pending_local_conflict) const {
    return session_detail::validate_native_landing(*this, player, id, planet, pending_local_conflict);
}
void GalacticSession::Impl::execute_landing(const GameCommand& command, const std::uint64_t tick,
    std::vector<GalacticEvent>& events, const bool pending_local_conflict) {
    const auto& order = std::get<LandFleet>(command.payload);
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat, order.fleet,
        command.key.sequence, order.planet, GalacticRejectReason::none};
    const auto reject = [&](GalacticRejectReason reason) { event.kind = GalacticEventKind::command_rejected; event.reason = reason; events.push_back(event); };
    const auto* issuer = seat(command.key.seat);
    if (!issuer || !issuer->active) return reject(GalacticRejectReason::inactive_seat);
    if (const auto reason = validate_landing(issuer->player, order.fleet, order.planet, pending_local_conflict); reason != GalacticRejectReason::none) return reject(reason);
    const auto found = fleets.find(order.fleet);
    auto& fleet = registry.get<session_detail::FleetData>(found->second);
    const auto* supplied = find_row(control->planets, order.planet, [](const auto& row) { return row.planet; });
    const PlayerId ground = supplied->ground_base.level ? supplied->ground_base.owner
        : (supplied->landed.empty() ? 0 : supplied->landed.front().owner);
    // WGM-39/42/53: only the sourced empty-surface uncontested branch is owned here.
    // Defended and special hero invasions need the conflict-admission service.
    if (ground && !allied(*control, fleet.owner, ground)) {
        if (!conflict_requests_enabled) return reject(GalacticRejectReason::movement_blocked);
        // WGM-39/42: retain a transport as a live object; its qualified ground predicate
        // belongs to the payload owner. Flatten fleet containers, never substitute a
        // passenger's type for the transport's invasion identity (WGM-49).
        std::vector<FleetMember> invasion_members(fleet.members.rbegin(), fleet.members.rend());
        bool capable = false;
        while (!invasion_members.empty() && !capable) {
            const auto member = invasion_members.back(); invasion_members.pop_back();
            const auto* trait = find_row(setup.fleet_control->types, member.type, [](const auto& row) { return row.type; });
            const auto* input = type(*control, member.type);
            if (trait && trait->transport) {
                auto qualified = has_ground_capability(*control, member, movement_inputs ? movement_inputs.get() : nullptr);
                if (!qualified) return reject(GalacticRejectReason::unknown_value);
                capable = input && qualified.value()
                    && !std::binary_search(control->absent_forces.begin(), control->absent_forces.end(), member.id);
            } else if (movement_inputs) {
                const auto* contents = find_row(movement_inputs->contents, member.id, [](const auto& row) { return row.container; });
                if (contents) invasion_members.insert(invasion_members.end(), contents->children.rbegin(), contents->children.rend());
            }
        }
        if (!capable) return reject(GalacticRejectReason::invalid_membership);
        events.push_back(event);
        event.kind = GalacticEventKind::land_conflict_requested; events.push_back(event);
        return;
    }
    // WGS-187: surface linking retains existing owners and appends the arriving owner.
    const auto& current = registry.get<session_detail::PlanetData>(planets.at(order.planet));
    const bool hostile = !allied(*control, fleet.owner, current.owner);
    std::vector<FleetMember> landing;
    const auto available = control->max_ground_forces - supplied->landed.size();
    for (const auto& member : fleet.members) {
        const auto* trait = find_row(setup.fleet_control->types, member.type, [](const auto& row) { return row.type; });
        const auto* input = type(*control, member.type);
        if (trait && trait->transport && input && landing.size() < available) {
            auto qualified = has_ground_capability(*control, member, movement_inputs ? movement_inputs.get() : nullptr);
            if (!qualified) return reject(GalacticRejectReason::unknown_value);
            if (!hostile || qualified.value()) landing.push_back(member);
        }
    }
    if (landing.empty()) return reject(available ? GalacticRejectReason::invalid_membership : GalacticRejectReason::resource_limit);
    events.push_back(event);
    auto& planet = control->planets[static_cast<std::size_t>(supplied - control->planets.data())];
    if (hostile) {
        // WGM-53: clear hostile orbital base and specials before successful uncontested landing.
        if (planet.starbase.level && !allied(*control, fleet.owner, planet.starbase.owner)) planet.starbase = {};
        planet.special_control.clear(); planet.production_owner = {};
        fix_built_structures(planet.built_structures, fleet.owner);
        clear_control_structures(static_cast<std::size_t>(supplied - control->planets.data()));
        PoliticalControlEffect effect; effect.planet = order.planet; effect.stop_production = true;
        effect.old_owner = current.owner; effect.new_owner = current.owner; control_effects.push_back(effect);
    }
    for (const auto& member : landing) {
        const auto it = std::find(fleet.members.begin(), fleet.members.end(), member);
        fleet.members.erase(it); planet.landed.push_back({member.id, member.type, fleet.owner});
        relocate_actor(member.id, fleet.owner, order.planet, order.planet);
        events.push_back({tick, GalacticEventKind::member_detached, command.key.seat, member.id, command.key.sequence, order.fleet, GalacticRejectReason::none});
    }
    // WGEP-41 / WGUI-41/103: LandFleet creates the same live surface links as a placement commit;
    // reuse the one ordered observer writer (appended in member order) rather than a second path.
    publish_surface_arrivals(order.planet, std::span<const SurfaceForce>(planet.landed).last(landing.size()));
    if (fleet.members.empty()) {
        registry.destroy(found->second); fleets.erase(found); --live_object_count;
        lose_actor(order.fleet);
        events.push_back({tick, GalacticEventKind::fleet_destroyed, command.key.seat, order.fleet, command.key.sequence, 0, GalacticRejectReason::none});
    }
    dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
    invalidate_observer_projection();
    dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
    if (setup.movement) dirty[static_cast<std::size_t>(StateSection::movement)] = true;
    dirty[9] = true;
    population_stale = true;
}
void GalacticSession::set_control_effects_handler(ControlEffectsHandler handler) { impl_->control_handler = std::move(handler); }

namespace {
std::vector<std::uint8_t> invasion_words(std::initializer_list<std::uint64_t> words) {
    std::vector<std::uint8_t> bytes;
    for (const auto value : words) detail::put_u64(bytes, value);
    return bytes;
}
std::optional<std::vector<std::uint64_t>> invasion_words(std::span<const std::uint8_t> bytes) {
    if (bytes.size() % 8 || bytes.size() < 16 || bytes.size() > galactic_replay_max_payload) return {};
    std::vector<std::uint64_t> words;
    for (std::size_t at = 0; at < bytes.size(); at += 8) {
        std::uint64_t value{};
        for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(bytes[at + i]) << (i * 8);
        words.push_back(value);
    }
    if (words[0] != 1) return {};
    return words;
}
}
bool GalacticSession::native_invasion_available() const noexcept { return impl_->native_invasions_enabled; }
void GalacticSession::enable_native_invasions(bool enabled) { impl_->native_invasions_enabled = enabled; }
const GalacticSession::NativeInvasion* GalacticSession::native_invasion(std::uint64_t request) const noexcept {
    if (const auto* shadow = native_shadow()) return native_shadow_invasion(*shadow, request);
    const auto found = impl_->native_invasions.find(request);
    return found == impl_->native_invasions.end() ? nullptr : &found->second;
}
const GalacticSession::NativeInvasion* GalacticSession::pending_native_invasion() const noexcept {
    if (const auto* shadow = native_shadow()) return native_shadow_pending_invasion(*shadow);
    return impl_->native_invasion_queue.empty() ? nullptr : native_invasion(impl_->native_invasion_queue.front());
}
core::Result<GalacticSession::NativeInvasionReceipt> GalacticSession::admit_native_invasion(PlayerId player, FleetId id, bool pending_local_conflict) {
    return admit_native_invasion(player, id, {}, pending_local_conflict);
}
core::Result<GalacticSession::NativeInvasionReceipt> GalacticSession::admit_native_invasion(PlayerId player, FleetId id,
    std::span<const EntityId> retained_transports, bool pending_local_conflict) {
    using Result = core::Result<NativeInvasionReceipt>;
    auto& impl = *impl_;
    if (auto* shadow = native_shadow()) return stage_native_invasion(*shadow, player, id, retained_transports, pending_local_conflict);
    auto result = session_detail::admit_native_invasion_rows(impl, player, id, retained_transports, pending_local_conflict);
    if (!result || !result.value().admitted) return result;
    const auto& receipt = result.value();
    auto bytes = invasion_words({1, retained_transports.empty() ? 1U : 4U, player, id, pending_local_conflict,
        receipt.admitted, receipt.request, receipt.queued, static_cast<std::uint64_t>(receipt.reason)});
    for (const auto transport : retained_transports) detail::put_u64(bytes, transport);
    auto recorded = append_native_effect(NativeEffectKind::invasion, std::move(bytes));
    if (!recorded) return Result::failure(recorded.error());
    return result;
}
core::Result<GalacticSession::NativeInvasionReceipt> GalacticSession::admit_native_invasion_by_command(PlayerId player, FleetId id,
    std::span<const EntityId> retained_transports) {
    using Result = core::Result<NativeInvasionReceipt>;
    if (native_shadow()) return Result::failure(invalid<void>("an invasion command cannot run inside the native action phase").error());
    return session_detail::admit_native_invasion_rows(*impl_, player, id, retained_transports, false);
}
core::Result<void> GalacticSession::Impl::complete_undefended_invasion(NativeInvasion& request, std::vector<GalacticEvent>& events) {
    const auto* supplied = find_row(control->planets, request.planet, [](const auto& row) { return row.planet; });
    if (!supplied) return invalid<void>("queued invasion surface disappeared");
    const auto index = static_cast<std::size_t>(supplied - control->planets.data());
    // WGA-236: gather the original explicit identities that still resolve.
    std::vector<std::pair<FleetId, FleetMember>> live;
    for (const auto transport : request.transports) {
        if (std::binary_search(control->absent_forces.begin(), control->absent_forces.end(), transport.id)) continue;
        const auto landed = std::find_if(supplied->landed.begin(), supplied->landed.end(), [&](const auto& row) { return row.id == transport.id && row.owner == request.invader; });
        if (landed != supplied->landed.end()) { live.emplace_back(FleetId{}, FleetMember{landed->id, landed->type}); continue; }
        for (const auto& [fleet_id, handle] : fleets) {
            const auto& fleet = registry.get<session_detail::FleetData>(handle);
            const auto current = std::find_if(fleet.members.begin(), fleet.members.end(), [&](const auto& row) { return row.id == transport.id; });
            if (fleet.owner == request.invader && current != fleet.members.end()) {
                live.emplace_back(fleet_id, *current); break;
            }
        }
    }
    if (!live.empty()) {
        auto& surface = control->planets[index];
        // The same uncontested control cleanup as WGM-53/60, without a battle result.
        surface.ground_base = {};
        if (surface.starbase.level && !allied(*control, request.invader, surface.starbase.owner)) surface.starbase = {};
        surface.special_control.clear(); surface.production_owner = {};
        fix_built_structures(surface.built_structures, request.invader);
        clear_control_structures(index);
        std::vector<EntityId> departed;
        // WGS-188: an undefended invasion preserves the recorded defender's
        // remaining transports; battle-win cleanup uses the invader instead.
        for (const auto& row : surface.landed) if (row.owner != request.defender) departed.push_back(row.id);
        std::erase_if(surface.landed, [&](const auto& row) { return row.owner != request.defender; });
        unpublish_surface_departures(request.planet, departed);
        for (const auto& [fleet_id, transport] : live) {
            auto at = fleets.find(fleet_id); if (at == fleets.end()) continue;
            auto& fleet = registry.get<session_detail::FleetData>(at->second);
            // The existing surface link gates can refuse; completion is not a placement census.
            if (!surface.surface_accessible || surface.landed.size() >= control->max_ground_forces || fleet.orbit != request.planet || fleet.motion.transit) continue;
            const auto member = std::find(fleet.members.begin(), fleet.members.end(), transport);
            if (member == fleet.members.end()) continue;
            fleet.members.erase(member); surface.landed.push_back({transport.id, transport.type, request.invader});
            publish_surface_arrivals(request.planet, std::span<const SurfaceForce>(surface.landed).last(1));
            relocate_actor(transport.id, request.invader, request.planet, request.planet);
            events.push_back({completed_tick, GalacticEventKind::member_detached, {}, transport.id, request.request, fleet_id, GalacticRejectReason::none});
            if (fleet.members.empty()) {
                registry.destroy(at->second); fleets.erase(at); --live_object_count;
                lose_actor(fleet_id);
                events.push_back({completed_tick, GalacticEventKind::fleet_destroyed, {}, fleet_id, request.request, 0, GalacticRejectReason::none});
            }
        }
        update_production_control(index, completed_tick, events, true);
        sync_observer_control(index);
        PoliticalControlEffect effect; effect.planet = request.planet; effect.stop_production = true;
        effect.old_owner = control->state[index].current_player; effect.new_owner = effect.old_owner; control_effects.push_back(effect);
        dirty[static_cast<std::size_t>(StateSection::control)] = true;
        dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
        dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
        if (setup.movement) dirty[static_cast<std::size_t>(StateSection::movement)] = true;
        population_stale = true; invalidate_observer_projection();
    }
    request.state = NativeInvasion::State::completed; request.success = true;
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::dispatch_native_invasion(std::uint64_t number, EntityId conflict) {
    auto& impl = *impl_;
    auto* request = impl.native_invasions.contains(number) ? &impl.native_invasions.at(number) : nullptr;
    if (impl.failed || !request || request->state != NativeInvasion::State::pending || impl.native_invasion_queue.empty()
        || impl.native_invasion_queue.front() != number || (conflict && conflict >= impl.next_id)) return invalid<void>("native invasion dispatch requires its retained front queue entry and reserved conflict");
    if (conflict) { request->conflict = conflict; request->state = NativeInvasion::State::conflict; }
    else {
        // WGA-236 calls the existing resume route once when the galaxy is paused.
        if (impl.clock.pause_depth) { if (auto resumed = adjust_conflict_pause(false); !resumed) return resumed; }
        std::vector<GalacticEvent> events(impl.snapshot->events().begin(), impl.snapshot->events().end());
        const auto effects_before = impl.control_effects.size();
        if (auto completed = impl.complete_undefended_invasion(*request, events); !completed) return completed;
        impl.native_invasion_transport_count -= request->transports.size(); request->transports.clear();
        if (impl.control_handler && impl.control_effects.size() != effects_before) {
            const auto planets = impl.sorted_planets();
            if (auto delivered = impl.control_handler(std::span(impl.control_effects).subspan(effects_before), planets); !delivered) return delivered;
        }
        impl.native_invasion_queue.erase(impl.native_invasion_queue.begin());
        impl.snapshot = impl.make_snapshot(std::move(events));
        if (auto delivered = impl.deliver_actor_changes(); !delivered) return delivered;
        if (request->reader_released) impl.native_invasions.erase(number);
    }
    impl.dirty[static_cast<std::size_t>(StateSection::native_invasions)] = true;
    return append_native_effect(NativeEffectKind::invasion, invasion_words({1, 2, number, conflict}), NativeEffectPhase::world_service);
}
core::Result<void> GalacticSession::finish_native_invasion(EntityId conflict, PlayerId winner) {
    auto& impl = *impl_;
    if (impl.native_invasion_queue.empty()) return core::Result<void>::success();
    auto& request = impl.native_invasions.at(impl.native_invasion_queue.front());
    if (request.state != NativeInvasion::State::conflict || request.conflict != conflict) return core::Result<void>::success();
    if (winner != request.invader && winner != request.defender) return invalid<void>("native invasion completion has no matching actual winner");
    request.state = NativeInvasion::State::completed; request.success = winner == request.invader;
    impl.native_invasion_transport_count -= request.transports.size(); request.transports.clear();
    impl.native_invasion_queue.erase(impl.native_invasion_queue.begin());
    const auto number = request.request;
    if (request.reader_released) impl.native_invasions.erase(number);
    impl.dirty[static_cast<std::size_t>(StateSection::native_invasions)] = true;
    // The trusted world outcome/child journal already records this actual winner.
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::retire_native_invasion(std::uint64_t number) {
    if (auto* shadow = native_shadow()) return stage_native_retire_invasion(*shadow, number);
    auto& impl = *impl_;
    const auto found = impl.native_invasions.find(number);
    if (impl.failed || found == impl.native_invasions.end()) return invalid<void>("invasion reader release requires its retained identity");
    auto& request = found->second;
    if (request.reader_released) return core::Result<void>::success();
    request.reader_released = true;
    if (request.state == NativeInvasion::State::completed || request.state == NativeInvasion::State::unqueued) impl.native_invasions.erase(found);
    impl.dirty[static_cast<std::size_t>(StateSection::native_invasions)] = true;
    return append_native_effect(NativeEffectKind::invasion, invasion_words({1, 3, number}));
}
core::Result<void> GalacticSession::retire_native_invasion_by_command(std::uint64_t number) {
    if (native_shadow()) return invalid<void>("an invasion command cannot run inside the native action phase");
    auto& impl = *impl_;
    const auto found = impl.native_invasions.find(number);
    if (impl.failed || found == impl.native_invasions.end()) return invalid<void>("invasion reader release requires its retained identity");
    auto& request = found->second;
    if (request.reader_released) return core::Result<void>::success();
    request.reader_released = true;
    if (request.state == NativeInvasion::State::completed || request.state == NativeInvasion::State::unqueued) impl.native_invasions.erase(found);
    impl.dirty[static_cast<std::size_t>(StateSection::native_invasions)] = true;
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::Impl::replay_native_invasion(GalacticSession& world, const NativeEffect& effect) {
    const auto words = invasion_words(std::span<const std::uint8_t>(effect.payload));
    if (!words) return invalid<void>("native invasion effect framing is invalid");
    const auto& input = *words;
    if (effect.phase == NativeEffectPhase::barrier && ((input.size() == 9 && input[1] == 1) || (input.size() > 9 && input[1] == 4)) && input[2] && input[2] <= std::numeric_limits<PlayerId>::max()
        && input[4] <= 1 && input[5] == 1 && input[7] <= 1 && input[8] == 0) {
        auto receipt = world.admit_native_invasion(static_cast<PlayerId>(input[2]), input[3],
            std::span(input).subspan(9), input[4] != 0);
        if (!receipt) return core::Result<void>::failure(receipt.error());
        if (receipt.value() != NativeInvasionReceipt{true, input[6], input[7] != 0, static_cast<GalacticRejectReason>(input[8])}) return invalid<void>("native invasion admission differs from recorded receipt");
        return core::Result<void>::success();
    }
    if (effect.phase == NativeEffectPhase::world_service && input.size() == 4 && input[1] == 2) return world.dispatch_native_invasion(input[2], input[3]);
    if (effect.phase == NativeEffectPhase::barrier && input.size() == 3 && input[1] == 3) return world.retire_native_invasion(input[2]);
    return invalid<void>("native invasion effect does not match its recorded owner phase");
}
core::Result<void> GalacticSession::replay_native_invasion_service() {
    auto& impl = *impl_;
    if (!impl.native_replaying || impl.native_replay_cursor == impl.native_replay.size()) return core::Result<void>::success();
    const auto effect = impl.native_replay[impl.native_replay_cursor];
    if (effect.kind != NativeEffectKind::invasion || effect.phase != NativeEffectPhase::world_service || effect.frontier != impl.completed_tick) return core::Result<void>::success();
    auto consumed = consume_native_effect(NativeEffectKind::invasion); if (!consumed) return core::Result<void>::failure(consumed.error());
    struct Applying { bool& flag; explicit Applying(bool& value) : flag(value) { flag = true; } ~Applying() { flag = false; } } applying(impl.native_applying);
    return impl.replay_native_invasion(*this, effect);
}

core::Result<GalacticSession::NativeTransportLandingGate> GalacticSession::native_transport_landing_available(PlayerId player, PlanetId id,
    std::optional<std::uint32_t> live_landed) const {
    using Result = core::Result<NativeTransportLandingGate>;
    if (const auto* shadow = native_shadow()) return native_shadow_landing_available(*shadow, player, id, live_landed);
    const auto& impl = *impl_;
    if (!impl.control) return invalid<NativeTransportLandingGate>("automatic landing surface owner unavailable");
    const auto* surface = find_row(impl.control->planets, id, [](const auto& row) { return row.planet; });
    if (!surface) return invalid<NativeTransportLandingGate>("automatic landing surface identity unavailable");
    // WGA-416 / ADR-009: the world owner revalidates capacity at commit.
    // The publication's incremented count is only a checked staging hint.
    const auto actual_landed = landed_count(*surface, impl.actor_records);
    assert(!live_landed || *live_landed == actual_landed);
    (void)live_landed; // The hint has no authority in builds without assertions.
    if (actual_landed >= impl.control->max_ground_forces) return Result::success(NativeTransportLandingGate::capacity);
    if (!surface->surface_accessible) return Result::success(NativeTransportLandingGate::surface);
    // WGS-187: ownership safety belongs to the caller, not the surface link.
    // mixed_allied_owner_unavailable is a reserved diagnostic value, never emitted.
    return Result::success(NativeTransportLandingGate::admitted);
}
core::Result<GalacticSession::NativeTransportLandingReceipt> GalacticSession::land_native_transport(
    PlayerId player, EntityId actor, PlanetId id, bool pending_local_conflict, std::optional<std::uint32_t> live_landed) {
    using Result = core::Result<NativeTransportLandingReceipt>;
    auto& impl = *impl_;
    if (auto* shadow = native_shadow()) return stage_native_actor_land(*shadow, player, actor, id, pending_local_conflict, live_landed);
    std::vector<GalacticEvent> events;
    auto prepared = session_detail::land_native_transport_rows(impl, player, actor, id, pending_local_conflict, live_landed, events);
    if (!prepared) return Result::failure(prepared.error());
    const auto receipt = prepared.value().receipt;
    if (prepared.value().published) {
        const auto old = impl.snapshot->events();
        events.insert(events.begin(), old.begin(), old.end());
        impl.snapshot = impl.publish_native_snapshot(std::move(events));
    }
    if (auto saved = impl.journal_native_receipt(*this, NativeEffectKind::actor_land, player, actor, id,
        pending_local_conflict, nullptr, receipt.event); !saved) return Result::failure(saved.error());
    if (prepared.value().published) if (auto delivered = impl.deliver_actor_changes(); !delivered) return Result::failure(delivered.error());
    return Result::success(receipt);
}

core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::land_native_transports(PlayerId player, FleetId id,
    bool pending_local_conflict) {
    using Result = core::Result<NativeFleetReceipt>;
    auto& impl = *impl_;
    if (auto* shadow = native_shadow()) return stage_native_land(*shadow, player, id, pending_local_conflict);
    std::vector<GalacticEvent> events;
    auto prepared = session_detail::land_native_transports_rows(impl, player, id, pending_local_conflict, events);
    if (!prepared) return Result::failure(prepared.error());
    if (!prepared.value().published) return Result::success(prepared.value().receipt);
    const auto old = impl.snapshot->events();
    events.insert(events.begin(), old.begin(), old.end());
    impl.snapshot = impl.publish_native_snapshot(std::move(events));
    const NativeFleetReceipt receipt{true, id};
    if (auto journaled = impl.journal_native_receipt(*this, NativeEffectKind::land, player, id, 0,
        pending_local_conflict, nullptr, receipt); !journaled) return Result::failure(journaled.error());
    if (auto delivered = impl.deliver_actor_changes(); !delivered) return Result::failure(delivered.error());
    return Result::success(receipt);
}

core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::Impl::launch_surface_units(PlayerId player,
    std::span<const EntityId> units, FleetId target, std::uint8_t slot, const LaunchAdmission& admit,
    bool& admission_ran) {
    return session_detail::launch_native_surface_units(*this, player, units, target, slot, admit, admission_ran);
}

void GalacticSession::Impl::execute_launch(const GameCommand& command, const std::uint64_t tick,
    std::vector<GalacticEvent>& events, const bool pending_local_conflict) {
    const auto& order = std::get<LaunchFromSurface>(command.payload);
    GalacticEvent event{tick, GalacticEventKind::command_executed, command.key.seat, order.units.front(),
        command.key.sequence, order.target, GalacticRejectReason::none};
    const auto reject = [&](GalacticRejectReason reason) {
        event.kind = GalacticEventKind::command_rejected; event.reason = reason; events.push_back(event);
    };
    const auto* issuer = seat(command.key.seat);
    if (!issuer || !issuer->active) return reject(GalacticRejectReason::inactive_seat);
    if (!control || !setup.fleet_control) return reject(GalacticRejectReason::unknown_value);
    const auto first = events.size();
    bool admission_ran{};
    const auto launched = launch_surface_units(issuer->player, order.units, order.target, order.slot,
        [&](const CommandPayload& payload) {
            // The ordinary fleet owner publishes this command's own executed/rejected event.
            execute_fleet(GameCommand{command.key, payload}, tick, events, pending_local_conflict);
            NativeFleetReceipt receipt;
            for (auto i = first; i < events.size(); ++i) {
                if (events[i].kind == GalacticEventKind::command_rejected) receipt.reason = events[i].reason;
                else if (events[i].kind == GalacticEventKind::command_executed) {
                    receipt.admitted = true; receipt.object = events[i].subject;
                }
            }
            return core::Result<NativeFleetReceipt>::success(receipt);
        }, admission_ran);
    if (!admission_ran) return reject(launched ? launched.value().reason : GalacticRejectReason::invalid_membership);
}

void GalacticSession::Impl::execute_launch_fleet(const GameCommand& command, const std::uint64_t tick,
    std::vector<GalacticEvent>& events, const bool pending_local_conflict) {
    const auto& order = std::get<LaunchFleet>(command.payload);
    const auto* issuer = seat(command.key.seat);
    PlanetId source{};
    if (issuer && issuer->active && control) {
        for (const auto& planet : control->planets) {
            const auto unit = std::find_if(planet.landed.begin(), planet.landed.end(),
                [&](const auto& row) { return row.id == order.units.front(); });
            if (unit == planet.landed.end() || unit->owner != issuer->player) continue;
            source = planet.planet;
            // A malformed source path must not turn a surface unit into an orbiting one.
            if (order.path.front() != source) {
                events.push_back({tick, GalacticEventKind::command_rejected, command.key.seat, order.units.front(),
                    command.key.sequence, order.path.back(), GalacticRejectReason::invalid_path});
                return;
            }
            break;
        }
    }
    const auto begin = events.size();
    // WGM-199: use the surface owner and resolve its generated identity here,
    // never from a predicted object counter in presentation.
    execute_launch({command.key, LaunchFromSurface{order.units, 0, 2}}, tick, events, pending_local_conflict);
    FleetId id{};
    for (auto i = begin; i < events.size(); ++i) {
        if (events[i].kind != GalacticEventKind::command_executed) continue;
        // Ordinary fleet admission emits exactly one executed event on success, none on refusal.
        assert(!id && "surface fleet admission emitted multiple executed events");
        id = events[i].subject;
    }
    if (!id) return;
    assert(issuer && source);
    const auto cancel = [&](GalacticRejectReason reason) {
        events.push_back({tick, GalacticEventKind::movement_canceled, command.key.seat, id,
            command.key.sequence, order.path.back(), reason});
    };
    const auto linked = events.size();
    if (auto checked = recheck_movement_orbit(id, tick, events); !checked) {
        // Launch is committed. Only its travel continuation failed to obtain a movement owner.
        cancel(GalacticRejectReason::unknown_value);
        return;
    }
    // WGM-199: source-orbit conflict consumes the launch's movement continuation.
    if (std::any_of(events.begin() + static_cast<std::ptrdiff_t>(linked), events.end(),
        [](const auto& event) { return event.kind == GalacticEventKind::movement_conflict; })) return;
    // An absent destination is not a qualified entry result. Keep the committed
    // launch but cancel travel for every issuer, including nonhuman owners.
    if (!planets.contains(order.path.back())) return cancel(GalacticRejectReason::unknown_value);
    const bool human = std::any_of(seats.begin(), seats.end(), [&](const auto& row) {
        return row.player == issuer->player && row.kind == SeatKind::human;
    });
    const auto* entry = human ? find_row(movement_inputs->access, std::pair{issuer->player, order.path.back()},
        [](const auto& row) { return std::pair{row.player, row.planet}; }) : nullptr;
    if (human && !entry) return cancel(GalacticRejectReason::unknown_value);
    // WGM-200: this per-player result is prequalified by the entry owner, including
    // restriction/story exceptions. It is not the planet's raw restriction flag.
    if (human && !entry->accessible) {
        std::vector<GalacticEvent> removal;
        execute_fleet({command.key, RemoveFleetMembers{id, order.units}}, tick, removal, pending_local_conflict);
        const auto refused = std::find_if(removal.begin(), removal.end(), [](const auto& event) {
            return event.kind == GalacticEventKind::command_rejected;
        });
        // The just-admitted group is wholly owned and attached to this fleet, so
        // removal cannot refuse. Keep a cancellation if that admission invariant changes.
        if (refused != removal.end()) return cancel(refused->reason);
        for (const auto& event : removal)
            if (event.kind != GalacticEventKind::command_executed) events.push_back(event);
        auto surface = std::find_if(control->planets.begin(), control->planets.end(),
            [source](const auto& row) { return row.planet == source; });
        assert(surface != control->planets.end());
        std::vector<SurfaceForce> arrivals;
        arrivals.reserve(order.units.size());
        for (const auto actor : order.units) {
            const auto& unit = detached_units.at(actor);
            arrivals.push_back({actor, unit.member.type, unit.owner});
        }
        // WGM-199: the leading unit re-lands first; followers see its surface parent
        // and append there in order. Restore links without a fresh landing/capacity gate.
        surface->landed.insert(surface->landed.end(), arrivals.begin(), arrivals.end());
        for (const auto& unit : arrivals) {
            relocate_actor(unit.id, unit.owner, source, source);
            detached_units.erase(unit.id);
        }
        publish_surface_arrivals(source, arrivals);
        dirty[static_cast<std::size_t>(StateSection::control)] = true;
        population_stale = true;
        invalidate_observer_projection();
        cancel(GalacticRejectReason::movement_blocked);
        return;
    }
    execute_movement({command.key, MoveFleet{id, order.path, order.destination_slot}}, tick, events, pending_local_conflict);
}

core::Result<GalacticSession::NativeFleetReceipt> GalacticSession::launch_native_transport(PlayerId player,
    EntityId id, FleetId target, bool pending_local_conflict) {
    using Result = core::Result<NativeFleetReceipt>;
    auto& impl = *impl_;
    if (impl.failed || !impl.control || !impl.setup.fleet_control)
        return invalid<NativeFleetReceipt>("native launch requires its live control owner");
    if (auto* shadow = native_shadow()) return stage_native_launch(*shadow, player, id, target, pending_local_conflict);
    const auto restore_view = [&] {
        // Rejected admission published the temporary detached row. Restore
        // the unchanged completed view as well as the authoritative table.
        impl.snapshot = impl.publish_native_snapshot(std::vector<GalacticEvent>(impl.snapshot->events().begin(), impl.snapshot->events().end()));
    };
    bool admission_ran{};
    const std::array<EntityId, 1> units{id};
    auto launched = impl.launch_surface_units(player, units, target, 0, [&](const CommandPayload& payload) {
        // The logical launch owns its temporary detached link; nested fleet
        // admission must not record an effect lacking that link on replay.
        const auto previous_applying = impl.native_applying;
        impl.native_applying = true;
        auto accepted = admit_native_fleet(player, payload, pending_local_conflict);
        impl.native_applying = previous_applying;
        return accepted;
    }, admission_ran);
    if (!launched) { if (admission_ran) restore_view(); return launched; }
    if (!admission_ran) return launched; // gate refusal: nothing published or journaled
    if (!launched.value().admitted) {
        restore_view();
        if (auto journaled = impl.journal_native_receipt(*this, NativeEffectKind::launch, player, id, target,
            pending_local_conflict, nullptr, launched.value()); !journaled) return Result::failure(journaled.error());
        return launched;
    }
    restore_view();
    if (auto journaled = impl.journal_native_receipt(*this, NativeEffectKind::launch, player, id, target,
        pending_local_conflict, nullptr, launched.value()); !journaled) return Result::failure(journaled.error());
    if (auto delivered = impl.deliver_actor_changes(); !delivered) return Result::failure(delivered.error());
    return launched;
}

core::Result<void> GalacticSession::add_tactical_built_structure(PlanetId planet,
    const TacticalBuiltStructure& record, PlayerId holder_owner) {
    auto& impl = *impl_;
    if (!impl.control) return invalid<void>("tactical persistence requires a galactic control parent");
    const auto* supplied = find_row(impl.control->planets, planet, [](const auto& p) { return p.planet; });
    if (!supplied || !record.type || record.enable_level > 5 || holder_owner != record.owner
        || !find_row(impl.control->players, record.owner, [](const auto& p) { return p.player; }))
        return invalid<void>("invalid tactical built-object writer context (WGT-84)");
    std::size_t total{};
    for (const auto& p : impl.control->planets)
        total += p.built_structures.size() + p.landed.size() + p.special_control.size() + p.caps.size() + p.transition_modifiers.size();
    if (total >= galactic_max_objects) return invalid<void>("tactical persistence exceeds object budget");
    if (auto journaled = append_native_effect(NativeEffectKind::tactical_built, built_structure_receipt(true, planet, record, holder_owner)); !journaled)
        return journaled;
    impl.control->planets[static_cast<std::size_t>(supplied - impl.control->planets.data())].built_structures.push_back(record);
    impl.dirty[9] = true;
    impl.snapshot = impl.make_snapshot(std::vector<GalacticEvent>(impl.snapshot->events().begin(), impl.snapshot->events().end()));
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::remove_tactical_built_structure(PlanetId planet, TypeId original, math::Vec3 position) {
    auto& impl = *impl_;
    if (!impl.control) return invalid<void>("tactical persistence requires a galactic control parent");
    const auto* supplied = find_row(impl.control->planets, planet, [](const auto& p) { return p.planet; });
    if (!supplied || !original) return invalid<void>("invalid tactical built-object removal context");
    auto& records = impl.control->planets[static_cast<std::size_t>(supplied - impl.control->planets.data())].built_structures;
    const auto at = std::find_if(records.begin(), records.end(), [&](const auto& r) { return r.type == original && r.position == position; });
    if (at == records.end()) return core::Result<void>::success();
    if (auto journaled = append_native_effect(NativeEffectKind::tactical_built,
        built_structure_receipt(false, planet, {original, position, 0, 0}, 0)); !journaled) return journaled;
    records.erase(at);
    impl.dirty[9] = true;
    impl.snapshot = impl.make_snapshot(std::vector<GalacticEvent>(impl.snapshot->events().begin(), impl.snapshot->events().end()));
    return core::Result<void>::success();
}
core::Result<void> GalacticSession::fix_tactical_built_structures(PlanetId planet, PlayerId winner) {
    auto& impl = *impl_;
    if (!impl.control) return core::Result<void>::success();
    const auto* supplied = find_row(impl.control->planets, planet, [](const auto& p) { return p.planet; });
    if (!supplied || !find_row(impl.control->players, winner, [](const auto& p) { return p.player; }))
        return invalid<void>("invalid tactical persistence winner");
    auto& records = impl.control->planets[static_cast<std::size_t>(supplied - impl.control->planets.data())].built_structures;
    const auto before = records.size(); fix_built_structures(records, winner);
    if (records.size() == before) return core::Result<void>::success();
    impl.dirty[9] = true;
    impl.snapshot = impl.make_snapshot(std::vector<GalacticEvent>(impl.snapshot->events().begin(), impl.snapshot->events().end()));
    return core::Result<void>::success();
}

core::Result<void> GalacticSession::apply_control_battle(const ControlBattleReturn& returned) {
    auto& impl = *impl_;
    if (!impl.control) return core::Result<void>::success();
    const auto* supplied = find_row(impl.control->planets, returned.planet, [](const auto& p) { return p.planet; });
    const auto known = [&](PlayerId id) { return find_row(impl.control->players, id, [](const auto& p) { return p.player; }); };
    if (!supplied || !known(returned.invader) || !known(returned.defender) || returned.invader == returned.defender
        || (returned.winner != returned.invader && returned.winner != returned.defender)
        || returned.destroyed.size() > galactic_max_objects || returned.surviving_invaders.size() > galactic_max_objects
        || (!returned.land && !returned.surviving_invaders.empty()) || !ordered(returned.destroyed, [](auto id) { return id; })
        || std::any_of(returned.destroyed.begin(), returned.destroyed.end(),
            [&](auto id) { return id == 0 || id >= impl.next_id; }))
        return invalid<void>("invalid control battle return context");
    const auto index = static_cast<std::size_t>(supplied - impl.control->planets.data());
    auto planet = *supplied;
    std::set<EntityId> destroyed(returned.destroyed.begin(), returned.destroyed.end()), survivors;
    auto absent = impl.control->absent_forces;
    for (const auto id : returned.destroyed) {
        const auto it = std::lower_bound(absent.begin(), absent.end(), id);
        if (it == absent.end() || *it != id) absent.insert(it, id);
    }
    if (absent.size() > galactic_max_objects) return invalid<void>("control result exceeds absent roster budget");
    for (const auto& force : returned.surviving_invaders) {
        const auto& traits = impl.setup.fleet_control;
        const bool transport = traits && std::any_of(traits->types.begin(), traits->types.end(),
            [&](const auto& row) { return row.type == force.type && row.transport; });
        if (force.id == 0 || force.owner != returned.invader || !type(*impl.control, force.type)
            || !transport || destroyed.contains(force.id)
            || std::binary_search(impl.control->absent_forces.begin(), impl.control->absent_forces.end(), force.id)
            || !survivors.insert(force.id).second)
            return invalid<void>("invalid control invasion survivor");
        bool live = std::find(planet.landed.begin(), planet.landed.end(), force) != planet.landed.end();
        for (const auto& [id, handle] : impl.fleets) {
            (void)id; const auto& fleet = impl.registry.get<session_detail::FleetData>(handle);
            if (fleet.owner == force.owner && fleet.orbit == returned.planet
                && std::find(fleet.members.begin(), fleet.members.end(), FleetMember{force.id, force.type}) != fleet.members.end()) live = true;
        }
        if (!live) return invalid<void>("invasion survivor is not a live source identity at the conflict planet");
    }
    std::vector<EntityId> departed;
    std::vector<SurfaceForce> arrived;
    for (const auto& force : planet.landed) if (destroyed.contains(force.id)) departed.push_back(force.id);
    std::erase_if(planet.landed, [&](const auto& force) { return destroyed.contains(force.id); });
    // WGM-57/60: returned levels can decrease; land victory replaces military
    // presence through surviving transport identities, not a political assignment.
    if (returned.land && returned.winner == returned.invader) {
        planet.ground_base = {};
        if (planet.starbase.owner == returned.defender) planet.starbase = {};
        planet.special_control.clear(); planet.production_owner = {};
        impl.clear_control_structures(index);
        for (const auto& force : planet.landed) if (force.owner != returned.invader) departed.push_back(force.id);
        std::erase_if(planet.landed, [&](const auto& force) { return force.owner != returned.invader; });
        for (const auto& force : returned.surviving_invaders) {
            if (planet.landed.size() >= impl.control->max_ground_forces) break;
            if (std::none_of(planet.landed.begin(), planet.landed.end(), [&](const auto& row) { return row.id == force.id; })) {
                planet.landed.push_back(force); arrived.push_back(force);
            }
        }
    } else if (!returned.land && planet.starbase.owner == (returned.station_owner ? returned.station_owner : returned.defender)) {
        planet.starbase.level = std::min(planet.starbase.level, returned.station_level);
        if (!planet.starbase.level) planet.starbase = {};
    }
    if (returned.land) fix_built_structures(planet.built_structures, returned.winner);
    impl.control->planets[index] = std::move(planet);
    impl.unpublish_surface_departures(returned.planet, departed);
    impl.publish_surface_arrivals(returned.planet, arrived);
    for (auto id : returned.destroyed) impl.lose_actor(id);
    for (const auto& force : impl.control->planets[index].landed)
        impl.relocate_actor(force.id, force.owner, returned.planet, returned.planet);
    impl.control->absent_forces = std::move(absent);
    std::vector<GalacticEvent> return_events;
    impl.update_production_control(index, impl.completed_tick, return_events,
        returned.land && returned.winner == returned.invader);
    impl.sync_observer_control(index); // cleared bases and landed survivors reach the observer at once
    if (returned.land && returned.winner == returned.invader) {
        const auto& landed = impl.control->planets[index].landed;
        for (auto it = impl.fleets.begin(); it != impl.fleets.end();) {
            auto& fleet = impl.registry.get<session_detail::FleetData>(it->second);
            std::erase_if(fleet.members, [&](const auto& member) {
                return std::any_of(landed.begin(), landed.end(), [&](const auto& force) { return force.id == member.id; });
            });
            if (fleet.members.empty()) { impl.lose_actor(it->first); impl.registry.destroy(it->second); it = impl.fleets.erase(it); --impl.live_object_count; }
            else ++it;
        }
        PoliticalControlEffect effect; effect.planet = returned.planet; effect.stop_production = true;
        effect.old_owner = impl.control->state[index].current_player; effect.new_owner = effect.old_owner;
        impl.control_effects = {effect};
        // The callback may publish a snapshot of the committed return roster.
        impl.invalidate_income_inputs();
        if (impl.control_handler) {
            const auto current_planets = impl.sorted_planets();
            if (auto applied = impl.control_handler(impl.control_effects, current_planets); !applied) return applied;
        }
    }
    impl.dirty[9] = true;
    impl.dirty[static_cast<std::size_t>(StateSection::fleets)] = true;
    impl.invalidate_observer_projection();
    if (impl.setup.fleet_control) impl.dirty[static_cast<std::size_t>(StateSection::fleet_control)] = true;
    if (impl.setup.movement) impl.dirty[static_cast<std::size_t>(StateSection::movement)] = true;
    // Reconciliation already retained this return step's session controls.
    // Publish their events before the ordered military/production return events.
    std::vector<GalacticEvent> events(impl.snapshot->events().begin(), impl.snapshot->events().end());
    events.insert(events.end(), return_events.begin(), return_events.end());
    impl.snapshot = impl.make_snapshot(std::move(events));
    return impl.deliver_actor_changes();
}
} // namespace eawr::sim::galactic

namespace eawr::sim::galactic::detail {
void encode_control(std::vector<std::uint8_t>& bytes, const PoliticalControlSetup& s) {
    const auto count = [&](auto size) { put_u32(bytes, static_cast<std::uint32_t>(size)); };
    const auto flag = [&](bool value) { put_u8(bytes, value ? 1 : 0); };
    const auto fixed = [&](math::Fixed value) { put_i64(bytes, value.raw()); };
    const auto base = [&](const ControlBase& value) { put_u32(bytes, value.owner); put_u32(bytes, value.level); };
    fixed(s.change_seconds); put_u32(bytes, s.max_ground_forces);
    count(s.players.size());
    for (const auto& r : s.players) { put_u32(bytes, r.player); flag(r.neutral); flag(r.playable); put_u32(bytes, r.faction); }
    count(s.relations.size());
    for (const auto& r : s.relations) { put_u32(bytes, r.player); put_u32(bytes, r.other); flag(r.allied); }
    count(s.types.size());
    for (const auto& r : s.types) { put_u64(bytes, r.type); put_u8(bytes, r.political_control); flag(r.stealth); flag(r.ground_capable); }
    count(s.planets.size());
    for (const auto& p : s.planets) {
        put_u64(bytes, p.planet); put_u64(bytes, p.type);
        flag(p.surface_accessible); flag(p.retains_residual); flag(p.destroyed); flag(p.protected_control);
        base(p.ground_base); base(p.starbase);
        count(p.landed.size());
        for (const auto& r : p.landed) { put_u64(bytes, r.id); put_u64(bytes, r.type); put_u32(bytes, r.owner); }
        for (auto owner : p.production_owner) put_u32(bytes, owner);
        put_u32(bytes, p.leaning_player); fixed(p.leaning);
        count(p.special_control.size()); for (auto value : p.special_control) fixed(value);
        count(p.caps.size()); for (const auto& r : p.caps) { put_u32(bytes, r.player); put_u8(bytes, r.maximum); }
        count(p.transition_modifiers.size());
        for (const auto& r : p.transition_modifiers) { put_u32(bytes, r.category); put_u64(bytes, r.source); fixed(r.multiplier); put_u64(bytes, r.expires_frame); }
        fixed(p.capture_reward);
        count(p.built_structures.size());
        for (const auto& r : p.built_structures) {
            put_u64(bytes, r.type); fixed(r.position.x); fixed(r.position.y); fixed(r.position.z);
            put_u32(bytes, r.enable_level); put_u32(bytes, r.owner);
        }
    }
    count(s.routes.size());
    for (const auto& r : s.routes) { put_u64(bytes, r.route); fixed(r.political_gain); }
    count(s.state.size());
    for (const auto& r : s.state) {
        put_u64(bytes, r.planet); flag(r.initialized);
        put_u32(bytes, r.current_player); put_u32(bytes, r.target_player); put_u32(bytes, r.controller); put_u32(bytes, r.orbital_player);
        fixed(r.current); fixed(r.target); flag(r.contested); flag(r.blockaded);
        fixed(r.political_income);
    }
    count(s.captured_types.size());
    for (const auto& r : s.captured_types) { put_u32(bytes, r.player); put_u64(bytes, r.type); }
    count(s.absent_forces.size()); for (auto id : s.absent_forces) put_u64(bytes, id);
    if (!s.ground_capability_types.empty()) {
        put_u8(bytes, 1); count(s.ground_capability_types.size());
        for (const auto& row : s.ground_capability_types) {
            put_u64(bytes, row.type); fixed(row.combat_power); flag(row.transport); flag(row.stays_in_transport);
        }
    }
}

core::Result<PoliticalControlSetup> decode_control(const std::span<const std::uint8_t> bytes) {
    sim::detail::Reader in(bytes); PoliticalControlSetup s;
    const auto fail = [&] { return core::Result<PoliticalControlSetup>::failure(diagnostic(diagnostic_codes::replay_malformed, "invalid political control extension")); };
    const auto fixed = [&](math::Fixed& value) { std::int64_t raw{}; if (!in.read_i64(raw)) return false; value = math::Fixed::from_raw(raw); return true; };
    const auto flag = [&](bool& value) { std::uint8_t raw{}; if (!in.read_u8(raw) || raw > 1) return false; value = raw != 0; return true; };
    const auto count = [&](std::uint32_t& value, std::size_t width, std::size_t limit = galactic_max_objects) {
        return in.read_u32(value) && value <= limit && std::size_t{value} * width <= in.remaining();
    };
    const auto base = [&](ControlBase& value) { return in.read_u32(value.owner) && in.read_u32(value.level); };
    std::uint32_t n{};
    if (!fixed(s.change_seconds) || !in.read_u32(s.max_ground_forces) || !count(n, 10, galactic_max_players)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) {
        ControlPlayer r; if (!in.read_u32(r.player) || !flag(r.neutral) || !flag(r.playable) || !in.read_u32(r.faction)) return fail(); s.players.push_back(r);
    }
    if (!count(n, 9, galactic_max_players * galactic_max_players)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) {
        ControlRelation r; if (!in.read_u32(r.player) || !in.read_u32(r.other) || !flag(r.allied)) return fail(); s.relations.push_back(r);
    }
    if (!count(n, 11)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) {
        ControlType r; if (!in.read_u64(r.type) || !in.read_u8(r.political_control) || !flag(r.stealth) || !flag(r.ground_capable)) return fail(); s.types.push_back(r);
    }
    if (!count(n, 80)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) {
        ControlPlanet p; std::uint32_t nested{};
        if (!in.read_u64(p.planet) || !in.read_u64(p.type) || !flag(p.surface_accessible) || !flag(p.retains_residual)
            || !flag(p.destroyed) || !flag(p.protected_control) || !base(p.ground_base) || !base(p.starbase) || !count(nested, 20)) return fail();
        for (std::uint32_t j = 0; j < nested; ++j) {
            SurfaceForce r; if (!in.read_u64(r.id) || !in.read_u64(r.type) || !in.read_u32(r.owner)) return fail(); p.landed.push_back(r);
        }
        if (!in.read_u32(p.production_owner[0]) || !in.read_u32(p.production_owner[1])
            || !in.read_u32(p.leaning_player) || !fixed(p.leaning) || !count(nested, 8, 3)) return fail();
        for (std::uint32_t j = 0; j < nested; ++j) { math::Fixed value; if (!fixed(value)) return fail(); p.special_control.push_back(value); }
        if (!count(nested, 5, galactic_max_players)) return fail();
        for (std::uint32_t j = 0; j < nested; ++j) { ControlCap r; if (!in.read_u32(r.player) || !in.read_u8(r.maximum)) return fail(); p.caps.push_back(r); }
        if (!count(nested, 28)) return fail();
        for (std::uint32_t j = 0; j < nested; ++j) {
            ControlTransitionModifier r;
            if (!in.read_u32(r.category) || !in.read_u64(r.source) || !fixed(r.multiplier) || !in.read_u64(r.expires_frame)) return fail();
            p.transition_modifiers.push_back(r);
        }
        if (!fixed(p.capture_reward) || !count(nested, 40)) return fail();
        for (std::uint32_t j = 0; j < nested; ++j) {
            TacticalBuiltStructure r;
            if (!in.read_u64(r.type) || !r.type || !fixed(r.position.x) || !fixed(r.position.y) || !fixed(r.position.z)
                || !in.read_u32(r.enable_level) || r.enable_level > 5 || !in.read_u32(r.owner) || !r.owner) return fail();
            p.built_structures.push_back(r);
        }
        s.planets.push_back(std::move(p));
    }
    if (!count(n, 16)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) { ControlRoute r; if (!in.read_u64(r.route) || !fixed(r.political_gain)) return fail(); s.routes.push_back(r); }
    if (!count(n, 51)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) {
        PlanetControl r;
        if (!in.read_u64(r.planet) || !flag(r.initialized) || !in.read_u32(r.current_player) || !in.read_u32(r.target_player)
            || !in.read_u32(r.controller) || !in.read_u32(r.orbital_player) || !fixed(r.current) || !fixed(r.target)
            || !flag(r.contested) || !flag(r.blockaded) || !fixed(r.political_income)) return fail();
        s.state.push_back(r);
    }
    if (!count(n, 12)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) { CapturedPlanetType r; if (!in.read_u32(r.player) || !in.read_u64(r.type)) return fail(); s.captured_types.push_back(r); }
    if (!count(n, 8)) return fail();
    for (std::uint32_t i = 0; i < n; ++i) { EntityId id{}; if (!in.read_u64(id)) return fail(); s.absent_forces.push_back(id); }
    if (in.remaining()) {
        std::uint8_t version{};
        if (!in.read_u8(version) || version != 1 || !count(n, 18) || !n || n != s.types.size()) return fail();
        for (std::uint32_t i = 0; i < n; ++i) {
            GroundCapabilityType row;
            if (!in.read_u64(row.type) || row.type != s.types[i].type || !fixed(row.combat_power)
                || !flag(row.transport) || !flag(row.stays_in_transport)) return fail();
            s.ground_capability_types.push_back(row);
        }
    }
    if (in.remaining()) return fail();
    return core::Result<PoliticalControlSetup>::success(std::move(s));
}
} // namespace eawr::sim::galactic::detail
