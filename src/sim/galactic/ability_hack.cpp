#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/ability_host.hpp"
#include "observer_internal.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>

// WGCH-57/66/109..112/139/148: live superweapon hacking on the shared ability host. Mutations follow the
// sourced order; every refusal that WGCH-112 places before the debit happens before any state change.
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

// WGCH-66/109: first exact CURRENT type match in stored orbit order, no owner/hostility/corruption filter.
std::optional<GalacticSession::Impl::HackWeapon> GalacticSession::Impl::find_hack_weapon(const PlanetId planet) const {
    if (!setup.ability_host || !setup.ability_host->hack_required_type) return std::nullopt;
    std::vector<Fleet> orbit;
    for (auto& fleet : sorted_fleets()) if (fleet.orbit == planet && !fleet.motion.transit) orbit.push_back(std::move(fleet));
    std::stable_sort(orbit.begin(), orbit.end(), [](const auto& a, const auto& b) { return a.motion.orbit_entry < b.motion.orbit_entry; });
    for (const auto& fleet : orbit) for (const auto& member : fleet.members) {
        const auto record = actor_records.find(member.id);
        const auto current = record == actor_records.end() ? member.type : record->second.current_type;
        if (record != actor_records.end() && (record->second.dead || record->second.delete_pending)) continue;
        if (current == setup.ability_host->hack_required_type) return HackWeapon{member.id, current, fleet.owner};
    }
    return std::nullopt;
}

// WGCH-141: hacking's orbital cleanup uses the reverse-container/named-hero pattern, every faction included.
bool GalacticSession::Impl::plan_orbit_loss(const PlanetId planet, const std::span<const EntityId> excluded,
    std::vector<HeroDefeat>& losses) {
    if (!setup.ability_host) return false;
    std::vector<Fleet> orbit;
    for (auto& fleet : sorted_fleets()) if (fleet.orbit == planet && !fleet.motion.transit) orbit.push_back(std::move(fleet));
    std::stable_sort(orbit.begin(), orbit.end(), [](const auto& a, const auto& b) { return a.motion.orbit_entry < b.motion.orbit_entry; });
    const auto skipped = [&](const EntityId id) { return std::find(excluded.begin(), excluded.end(), id) != excluded.end(); };
    for (auto fleet = orbit.rbegin(); fleet != orbit.rend(); ++fleet) {
        for (auto member = fleet->members.rbegin(); member != fleet->members.rend(); ++member) {
            const auto record = actor_records.find(member->id);
            if (skipped(member->id) || record == actor_records.end() || record->second.dead || record->second.delete_pending) continue;
            const auto* profile = find(setup.ability_host->profiles, record->second.original_type, [](const auto& r) { return r.type; });
            if (profile && profile->named) {
                auto defeat = prepare_ability_defeat(record->second, Fixed{}, false);
                if (!defeat) return false; // named parent-classification seam: refuse before any mutation
                defeat->current_system = defeat->planet; // WGM-80: no explicit planet
                defeat->planet = 0;
                std::vector<EntityId> work{member->id}; std::set<EntityId> visited;
                while (!work.empty()) {
                    const auto id = work.back(); work.pop_back();
                    if (!visited.insert(id).second) continue;
                    if (visited.size() > galactic_max_objects) return false;
                    if (id != member->id && !skipped(id)) defeat->removed_objects.push_back(id);
                    const auto* contents = movement_inputs ? find(movement_inputs->contents, id, [](const auto& r) { return r.container; }) : nullptr;
                    if (contents) for (const auto& child : contents->children) work.push_back(child.id);
                }
                losses.push_back(std::move(*defeat));
            } else if (!plan_container_loss(member->id, record->second.current_type, fleet->owner, losses, excluded)) return false;
        }
    }
    return true;
}

// The same ordered commit the control service applies to an owner change; a forced Neutral transition has
// no capture reward because the planet is already ruined (WGM-76, WGCH-150).
core::Result<void> GalacticSession::Impl::force_planet_owner(const PlanetId planet_id, const PlayerId new_owner,
    const std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto* surface = find(control->planets, planet_id, [](const auto& r) { return r.planet; });
    if (!surface) return core::Result<void>::success();
    const auto index = static_cast<std::size_t>(surface - control->planets.data());
    auto& state = control->state[index];
    const auto before = state;
    if (before.current_player == new_owner && before.target_player == new_owner && before.current == Fixed{}) return core::Result<void>::success();
    state.current_player = state.target_player = new_owner;
    state.current = state.target = Fixed{};
    dirty[static_cast<std::size_t>(StateSection::control)] = true;
    ++income_control_generation;
    PoliticalControlEffect effect; effect.planet = planet_id; effect.old_owner = before.current_player; effect.new_owner = new_owner;
    effect.owner_changed = before.current_player != new_owner;
    effect.stop_production = before.current >= Fixed::from_raw(Fixed::scale);
    effect.refresh_visibility = true;
    auto& planet = registry.get<session_detail::PlanetData>(planets.at(planet_id));
    if (effect.owner_changed) {
        --owned_planet_counts[planet.owner];
        ++owned_planet_counts[new_owner];
        planet.owner = new_owner; dirty[static_cast<std::size_t>(StateSection::planets)] = true;
        invalidate_income_inputs();
        relocate_actor(planet_id, new_owner, planet_id, 0);
        InlineExecutor inline_executor;
        const std::array kinds{VictoryKind::all_planets, VictoryKind::no_enemy_planets,
            VictoryKind::control_fraction, VictoryKind::homeworld_and_leader, VictoryKind::named_planets};
        if (auto tested = test_conditions(inline_executor, kinds, tick, events); !tested) return tested;
        refresh_credit_caps();
        events.push_back({tick, GalacticEventKind::planet_owner_changed, {}, planet_id, 0, new_owner, GalacticRejectReason::none});
    }
    sync_observer_control(index);
    if (effect.owner_changed) transfer_planet_spies(planet_id, effect.old_owner, new_owner);
    update_production_control(index, tick, events, effect.stop_production);
    if (effect.owner_changed) {
        auto corrupted = capture_corruption(planet_id, effect.old_owner, new_owner);
        if (!corrupted) return corrupted;
        if (auto refreshed = refresh_planet_technology_price(planet_id, true); !refreshed) return refreshed;
    }
    control_effects.push_back(effect);
    return core::Result<void>::success();
}

void GalacticSession::Impl::prepare_hack(const GameCommand& command, const CensusObject& actor,
    const GalacticAbilityProfile& profile, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    const auto target = std::get<PrepareGalacticAbility>(command.payload).planet;
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    const auto& host = *setup.ability_host;
    // WGCH-139: a named activated hack ability, a resolved required type and an exact current-type orbit match.
    // No opposing-planet, hostility, ruins or credit gate applies here (WGCH-109).
    if (!profile.hack || !planets.contains(target) || !host.hack_required_type
        || std::find(host.activated_hack_abilities.begin(), host.activated_hack_abilities.end(), profile.hack->name)
            == host.activated_hack_abilities.end()
        || !find_hack_weapon(target)) return reject();
    AbilityPreparation prepared; prepared.actor = actor.id; prepared.planet = target;
    prepared.kind = AbilityKind::superweapon_hack; prepared.open = true;
    prepared.offers.push_back({1, host.hack_cost, false}); // WGCH-66: authored price at preparation time
    prepared.id = ability_host->next_request++;
    if (auto* menu = find(economy_corruption->menus, actor.id, [](const auto& m) { return m.buyer; }); menu && menu->active) {
        menu->active = false; dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    }
    event.kind = GalacticEventKind::ability_prepared; event.value = prepared.id;
    put(ability_host->preparations, std::move(prepared), [](const auto& p) { return p.actor; });
    touch_abilities(); events.push_back(event);
}

void GalacticSession::Impl::execute_hack(const GameCommand& command, const CensusObject& actor,
    const PreparedAbility& prepared, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor.id, command.key.sequence};
    const auto reject = [&] { event.reason = GalacticRejectReason::unknown_value; events.push_back(event); };
    if (!control || !production || !economy || !ability_consumption_handler || prepared.planet == 0) return reject();
    const auto* surface = find(control->planets, prepared.planet, [](const auto& r) { return r.planet; });
    auto* account = find(economy->players, actor.owner, [](const auto& r) { return r.player; });
    const auto* player = find(economy_corruption->players, actor.owner, [](const auto& r) { return r.player; });
    if (!surface || !account || !player || !planets.contains(prepared.planet)) return reject();
    const auto index = static_cast<std::size_t>(surface - control->planets.data());
    // WGCH-112: the weapon match and affordability are re-resolved at execution and fail BEFORE any debit.
    // WGCH-110: the direct AI route pays nothing and has no affordability check.
    const auto weapon = find_hack_weapon(prepared.planet);
    const bool paid_route = player->human;
    if (!weapon || (paid_route && account->credits < prepared.price)) return reject();
    // Everything that can still refuse for an unresolved named seam is planned before the first mutation.
    const std::array<EntityId, 1> excluded{actor.id};
    std::optional<RuinsPlan> ruins;
    if (!surface->destroyed) {
        ruins = plan_planet_ruins(prepared.planet, true, excluded);
        if (!ruins) return reject();
    }
    std::vector<HeroDefeat> orbit_losses;
    if (!plan_orbit_loss(prepared.planet, excluded, orbit_losses)) return reject();
    std::optional<PlayerId> neutral;
    for (const auto& row : control->players) if (row.neutral) { neutral = row.player; break; }
    // The hacker leaves through the shared canonical loss with no named/structure metadata, so nothing is scheduled.
    HeroDefeat defeat; defeat.id = actor.id; defeat.type = defeat.respawn_type = actor.original_type;
    defeat.owner = actor.owner; defeat.planet = actor.planet;
    // Mutations: WGCH-66 debit first, then the owner lookup (a missing owner fails AFTER payment, no refund).
    if (paid_route && prepared.price > Fixed{}) {
        auto balance = math::subtract(account->credits, prepared.price), delta = math::negate(prepared.price);
        if (!balance || !delta) { failed = true; return; }
        if (credit_notification_handler) {
            auto notice = credit_notification_handler({CreditNotification::Kind::windfall, actor.owner, balance.value(), delta.value(), {}});
            if (!notice) { failed = true; return; }
        }
        account->credits = balance.value(); dirty[static_cast<std::size_t>(StateSection::players)] = true;
    }
    auto* holder = find(production->players, weapon->owner, [](const auto& r) { return r.player; });
    auto* retained = find(ability_host->preparations, actor.id, [](const auto& r) { return r.actor; });
    const auto settle = [&](AbilityOutcome outcome) {
        if (retained) retained->open = false;
        AbilityResult receipt{ability_host->next_request++, prepared.preparation, actor.id, prepared.planet, actor.owner, 0, outcome};
        receipt.kind = AbilityKind::superweapon_hack;
        put(ability_host->results, std::move(receipt), [](const auto& r) { return r.actor; });
        touch_abilities();
        event.kind = GalacticEventKind::ability_settled; event.value = prepared.preparation; events.push_back(event);
    };
    if (!holder) return settle(AbilityOutcome::refused);
    {   // The matched weapon's CURRENT type is locked for its CURRENT owner; no timer or later unlock exists.
        const auto at = std::lower_bound(holder->locked.begin(), holder->locked.end(), weapon->type);
        if (at == holder->locked.end() || *at != weapon->type) holder->locked.insert(at, weapon->type);
        dirty[static_cast<std::size_t>(StateSection::production)] = true;
    }
    // The hacker is removed with no explicit respawn schedule (WGCH-110).
    if (auto removed = ability_consumption_handler(defeat); !removed) { failed = true; return; }
    // WGCH-111: a 5 s corruption removal is requested when corrupted and is immediately aborted by ruins.
    const auto* economic = find(economy_corruption->planets, prepared.planet, [](const auto& r) { return r.planet; });
    if (economic && economic->corruption != corruption_fixture::Type::none) {
        auto requested = begin_corruption(prepared.planet, corruption_fixture::Type::none, Fixed::from_integer(5).value());
        if (!requested) { failed = true; return; }
    }
    if (ruins) {
        auto cleaned = execute_planet_ruins(*ruins);
        if (!cleaned) { failed = true; return; }
    }
    for (const auto& loss : orbit_losses) {
        if (auto lost = ability_consumption_handler(loss); !lost) { failed = true; return; }
    }
    // WGCH-66/111/142: hacking adds the starbase clear (WGEP-53 revalidates the space queue downstream).
    {
        auto& presence = control->planets[index];
        if (presence.starbase.level || presence.starbase.owner) { presence.starbase = {}; dirty[static_cast<std::size_t>(StateSection::control)] = true; }
        std::vector<GalacticEvent> discarded;
        update_production_control(index, tick, discarded, false);
        if (observer) if (auto* location = find(observer->planets, prepared.planet, [](const auto& r) { return r.planet; })) {
            location->space_level = 0; location->space_owner = 0; dirty[static_cast<std::size_t>(StateSection::observer)] = true;
        }
    }
    // WGCH-111: Neutral ownership is forced only when the named neutral faction and its player resolve.
    if (neutral) {
        auto forced = force_planet_owner(prepared.planet, *neutral, tick, events);
        if (!forced) { failed = true; return; }
    }
    population_stale = true; ++income_control_generation;
    invalidate_observer_projection();
    settle(AbilityOutcome::purchased);
}
} // namespace eawr::sim::galactic
