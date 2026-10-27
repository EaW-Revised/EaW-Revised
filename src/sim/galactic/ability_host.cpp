#include "eawr/sim/galactic/ability_host.hpp"
#include "session_impl.hpp"
#include "encoding.hpp"
#include "eawr/sim/galactic/observer.hpp"

#include <algorithm>
#include <limits>

namespace eawr::sim::galactic {
namespace {
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
bool ground_ability_enabled(const GalacticAbilityProfile& p, const AbilityActorState& s, AbilityKind kind) {
    if (p.lifecycle_only || p.type != s.type
        || std::find(p.ground_activated.begin(), p.ground_activated.end(), kind) == p.ground_activated.end()) return false;
    switch (kind) {
    case AbilityKind::system_spy: return p.spy && s.spy_enabled && !s.spy_cancelled;
    case AbilityKind::black_market: return p.has_market && s.enabled && !s.cancelled;
    case AbilityKind::technology_theft: return p.theft && s.theft_enabled && !s.theft_cancelled;
    case AbilityKind::corruption: return p.corruption && s.corruption_enabled && !s.corruption_cancelled;
    case AbilityKind::superweapon_hack: return p.hack && s.hack_enabled && !s.hack_cancelled;
    case AbilityKind::planet_destruction: return p.destruction && s.destruction_enabled && !s.destruction_cancelled;
    case AbilityKind::structure_sabotage: return p.sabotage && s.sabotage_enabled && !s.sabotage_cancelled;
    case AbilityKind::hero_neutralization: return p.neutralize && s.neutralize_enabled && !s.neutralize_cancelled;
    }
    return false;
}
void GalacticSession::Impl::dispatch_ground_ability(EntityId supplied, PlanetId target, SeatId producer,
    std::uint64_t sequence, std::uint64_t tick, std::vector<GalacticEvent>& events) {
    if (!ability_host || !setup.ability_host || !planets.contains(target)) return;
    const auto* issuer = seat(producer);
    if (!issuer || !issuer->active) return;
    const auto live = actor_records.find(supplied);
    if (live == actor_records.end() || live->second.dead || live->second.delete_pending
        || live->second.owner != issuer->player || live->second.planet != target) return;
    std::vector<EntityId> candidates{supplied};
    const auto* traits = movement_inputs ? find(movement_inputs->types, live->second.current_type,
        [](const auto& r) { return r.type; }) : nullptr;
    if (traits && traits->transport) {
        candidates.clear();
        const auto* contents = find(movement_inputs->contents, supplied, [](const auto& r) { return r.container; });
        if (contents) for (const auto& child : contents->children) candidates.push_back(child.id);
    }
    // WGCH-155/156: scan live contents in stored order, never retain a prepared bearer.
    for (const auto id : candidates) {
        const auto at = actor_records.find(id);
        if (at == actor_records.end() || at->second.dead || at->second.delete_pending
            || at->second.owner != issuer->player || at->second.planet != target
            || (id != supplied && at->second.container != supplied)) continue;
        const auto* profile = find(setup.ability_host->profiles, at->second.current_type, [](const auto& r) { return r.type; });
        const auto* service = find(ability_host->actors, id, [](const auto& r) { return r.actor; });
        if (!profile || !service) continue;
        bool qualified = false;
        for (const auto kind : profile->ground_activated) {
            if (!ground_ability_enabled(*profile, *service, kind)) continue;
            if (kind == AbilityKind::technology_theft || kind == AbilityKind::system_spy) {
                const auto* planet = control ? find(control->planets, target, [](const auto& row) { return row.planet; }) : nullptr;
                if (!planet) continue;
                const auto index = static_cast<std::size_t>(planet - control->planets.data());
                const auto& state = control->state[index];
                if (state.target_player == at->second.owner || std::any_of(control->relations.begin(), control->relations.end(), [&](const auto& relation) {
                    return relation.player == at->second.owner && relation.other == state.target_player && relation.allied;
                })) continue;
                if (kind == AbilityKind::technology_theft) {
                    const auto* current = economy_corruption ? find(economy_corruption->players, state.current_player, [](const auto& row) { return row.player; }) : nullptr;
                    if (!current || !current->playable || !production || !economy) continue;
                    const auto* purchaser = find(production->players, at->second.owner, [](const auto& row) { return row.player; });
                    const auto* account = find(economy->players, at->second.owner, [](const auto& row) { return row.player; });
                    // WGCH-55/155: an empty theft list is not an appropriate bearer.
                    // This admission reads no price, random draw or prepared absence.
                    if (!purchaser || !account) continue;
                    bool available = false;
                    for (const auto& choice : setup.ability_host->technologies) {
                        ++ability_work.registered_types;
                        const auto* type = find(production->technology_types, choice.type, [](const auto& row) { return row.type; });
                        if (type && type->initially_locked && type->slicer_unlockable && type->tech <= account->current_tech
                            && std::binary_search(type->affiliated_players.begin(), type->affiliated_players.end(), at->second.owner)
                            && !std::binary_search(purchaser->locked.begin(), purchaser->locked.end(), choice.type)
                            && !std::binary_search(purchaser->unlocked.begin(), purchaser->unlocked.end(), choice.type)) {
                            available = true; break;
                        }
                    }
                    if (!available) continue;
                }
            }
            qualified = true;
            const CommandOrderKey key{tick, producer, sequence};
            const auto begin = events.size();
            if (kind == AbilityKind::system_spy) {
                execute_spy({key, DeployGroundAbility{supplied, target}}, at->second, *profile, target, tick, events);
            } else if (kind == AbilityKind::planet_destruction) {
                // Bind the qualified live bearer; re-scanning the transport could select another child.
                PreparedAbility request; request.actor = id; request.planet = target; request.kind = kind;
                execute_ability({key, ExecutePreparedAbility{request}}, tick, events);
            } else execute_ability({key, PrepareGalacticAbility{id, target, kind}}, tick, events);
            if (std::any_of(events.begin() + static_cast<std::ptrdiff_t>(begin), events.end(), [](const auto& e) {
                return e.kind == GalacticEventKind::ability_prepared || e.kind == GalacticEventKind::ability_settled;
            })) return;
        }
        if (qualified) return; // first qualified contained object owns the submission, including refusal
    }
}
core::Result<void> validate_ability_host(const GalacticSetup& setup) {
    if (!setup.ability_host) return core::Result<void>::success();
    const auto bad = [] { return core::Result<void>::failure(detail::diagnostic(
        diagnostic_codes::invalid_setup, "live ability host requires canonical actor/content/economy inputs")); };
    const auto& host = *setup.ability_host;
    if (!setup.census || !setup.economy_corruption || !setup.player_economy
        || host.profiles.size() > galactic_max_objects) return bad(); // CGS-12: the default respawn time keeps any authored sign
    TypeId previous{};
    bool theft_enabled = false;
    for (const auto& profile : host.profiles) {
        const auto& market = profile.market;
        if (market.buyer || market.owner || !market.present || market.opposing_planet
            || market.despawn_requested || market.prepared_absence.raw()
            || (!profile.has_market && !profile.theft && !profile.corruption && !profile.lifecycle_only && !profile.hack && !profile.destruction
            && !profile.sabotage && !profile.neutralize && !profile.spy && profile.auto_spies.empty()) || !profile.type || profile.type <= previous || market.price_modifier.raw() < 0
            || market.absence_min.size() != market.absence_max.size()
            || (!market.absence_min.empty() && market.absence_min.size() != 5)
            || std::none_of(setup.census->types.begin(), setup.census->types.end(),
                [&](const auto& row) { return row.type == profile.type; })) return bad();
        if (profile.ground_activated.size() > 8) return bad();
        std::set<AbilityKind> group;
        for (const auto kind : profile.ground_activated)
            if (static_cast<unsigned>(kind) < 1 || static_cast<unsigned>(kind) > 8 || !group.insert(kind).second) return bad();
        if (profile.spy && (!setup.observer || !setup.control || !profile.spy->identity
            || profile.spy->name.empty() || profile.spy->name.size() > 4096 || profile.spy->name.find('\0') != std::string::npos
            || profile.spy->text_identity.size() > 1024 || profile.spy->text_identity.find('\0') != std::string::npos
            || (profile.spy->mask & ~all_intelligence) || profile.spy->duration <= math::Fixed{})) return bad();
        if (profile.auto_spies.size() > 8) return bad();
        for (const auto& automatic : profile.auto_spies)
            if (!setup.observer || !setup.control || !automatic.identity || automatic.text_identity.size() > 1024
                || automatic.text_identity.find('\0') != std::string::npos || (automatic.mask & ~all_intelligence)
                || automatic.duration >= math::Fixed{}) return bad(); // WGUI-19: automatic duration is validated to -1
        previous = profile.type;
        for (std::size_t i = 0; i < market.absence_min.size(); ++i)
            if (market.absence_min[i].raw() < 0 || market.absence_min[i] > market.absence_max[i]) return bad();
        if (profile.theft) {
            theft_enabled = true; const auto& t = *profile.theft;
            if (t.absence_min.size() != t.absence_max.size() || (!t.absence_min.empty() && t.absence_min.size() != 5)) return bad();
            for (std::size_t i = 0; i < t.absence_min.size(); ++i)
                if (t.absence_min[i] < math::Fixed{} || t.absence_min[i] > t.absence_max[i]) return bad();
            for (const auto* curve : {&t.availability, &t.ground, &t.space, &t.difficulty}) {
                if (curve->size() > galactic_max_objects) return bad();
                for (std::size_t i = 1; i < curve->size(); ++i) if ((*curve)[i - 1].x >= (*curve)[i].x) return bad();
            }
        }
    }
    if (host.technologies.size() > galactic_max_objects || host.theft_planets.size() > galactic_max_objects
        || (theft_enabled && (!setup.production || !setup.control))) return bad();
    std::set<TypeId> registered;
    for (const auto& t : host.technologies) {
        if (!t.type || !registered.insert(t.type).second || !setup.production
            || std::none_of(setup.production->technology_types.begin(), setup.production->technology_types.end(),
                [&](const auto& r) { return r.type == t.type; })) return bad();
    }
    PlanetId previous_planet{};
    for (const auto& p : host.theft_planets) {
        if (!p.planet || p.planet <= previous_planet || p.affinity_types.size() > galactic_max_objects
            || !std::is_sorted(p.affinity_types.begin(), p.affinity_types.end())
            || std::adjacent_find(p.affinity_types.begin(), p.affinity_types.end()) != p.affinity_types.end()
            || std::none_of(setup.planets.begin(), setup.planets.end(), [&](const auto& r) { return r.id == p.planet; })) return bad();
        previous_planet = p.planet;
        for (const auto id : p.affinity_types) if (!id || !setup.production
            || std::none_of(setup.production->technology_types.begin(), setup.production->technology_types.end(), [&](const auto& r) { return r.type == id; })) return bad();
    }
    const bool corruption = std::any_of(host.profiles.begin(), host.profiles.end(), [](const auto& p) { return p.corruption.has_value(); });
    if (host.activated_corruption_abilities.size() > galactic_max_objects || host.corruption_planets.size() > galactic_max_objects
        || host.corruption_players.size() > galactic_max_players || host.corruption_prerequisites.size() > galactic_max_objects
        || (corruption && (!setup.control || !setup.production || host.corruption_players.size() != setup.players.size()
            || host.corruption_planets.size() != setup.planets.size()))
        || (!host.corruption_players.empty() && host.corruption_planets.size() > galactic_max_objects / host.corruption_players.size())) return bad();
    for (const auto& name : host.activated_corruption_abilities) if (name.empty() || name.size() > 4096 || name.find('\0') != std::string::npos) return bad();
    for (const auto& p : host.profiles) if (p.corruption && (p.corruption->name.empty() || p.corruption->name.size() > 4096 || p.corruption->name.find('\0') != std::string::npos)) return bad();
    previous_planet = 0;
    for (const auto& p : host.corruption_planets) {
        if (!p.planet || p.planet <= previous_planet || !p.type
            || std::none_of(setup.planets.begin(), setup.planets.end(), [&](const auto& r) { return r.id == p.planet; })) return bad();
        previous_planet = p.planet;
        for (const auto& c : p.slots) {
            if (c.type > corruption_fixture::Type::bonded_citizens || c.required_mission_type > corruption_fixture::Type::bonded_citizens
                || c.price < math::Fixed{} || c.mode > 2 || c.unit_types.size() > galactic_max_objects || c.planet_types.size() > galactic_max_objects
                || c.map.size() > 4096 || c.script.size() > 4096 || c.briefing.size() > 4096
                || c.map.find('\0') != std::string::npos || c.script.find('\0') != std::string::npos || c.briefing.find('\0') != std::string::npos) return bad();
            for (const auto id : c.unit_types) if (!id || std::none_of(host.corruption_prerequisites.begin(), host.corruption_prerequisites.end(), [&](const auto& r) { return r.type == id; })) return bad();
            for (const auto id : c.planet_types) if (!id) return bad();
        }
    }
    PlayerId previous_player{};
    for (const auto& p : host.corruption_players) {
        if (!p.player || p.player <= previous_player || std::none_of(setup.players.begin(), setup.players.end(), [&](const auto& r) { return r.player_id == p.player; })) return bad();
        previous_player = p.player;
    }
    previous = 0;
    for (const auto& p : host.corruption_prerequisites) {
        if (!p.type || p.type <= previous || std::none_of(setup.census->types.begin(), setup.census->types.end(), [&](const auto& r) { return r.type == p.type; })) return bad();
        previous = p.type;
    }
    if (host.initial_corruption.size() > galactic_max_objects) return bad();
    for (const auto& r : host.initial_corruption) if (r.type > corruption_fixture::Type::bonded_citizens
        || std::none_of(host.corruption_planets.begin(), host.corruption_planets.end(), [&](const auto& p) { return p.planet == r.planet; })
        || std::none_of(host.corruption_players.begin(), host.corruption_players.end(), [&](const auto& p) { return p.player == r.recipient; })) return bad();
    // WGCH-109/110/139: authored hack constants and named activated abilities.
    if (host.hack_cost < math::Fixed{} || host.activated_hack_abilities.size() > galactic_max_objects) return bad();
    for (const auto& name : host.activated_hack_abilities)
        if (name.empty() || name.size() > 4096 || name.find('\0') != std::string::npos) return bad();
    for (const auto& p : host.profiles) if (p.hack && (p.hack->name.empty() || p.hack->name.size() > 4096
        || p.hack->name.find('\0') != std::string::npos || !setup.control || !setup.production)) return bad();
    if (host.activated_spy_abilities.size() > galactic_max_objects) return bad();
    for (const auto& name : host.activated_spy_abilities)
        if (name.empty() || name.size() > 4096 || name.find('\0') != std::string::npos) return bad();
    if (host.activated_destruction_abilities.size() > galactic_max_objects) return bad();
    for (const auto& name : host.activated_destruction_abilities)
        if (name.empty() || name.size() > 4096 || name.find('\0') != std::string::npos) return bad();
    for (const auto& p : host.profiles) if (p.destruction && (p.destruction->name.empty() || p.destruction->name.size() > 4096
        || p.destruction->name.find('\0') != std::string::npos || !setup.control || !setup.production)) return bad();
    // WGCH-27/28/97 and WGCH-20..24/52: sabotage and neutralization content tables and named abilities.
    if (host.activated_sabotage_abilities.size() > galactic_max_objects || host.activated_neutralize_abilities.size() > galactic_max_objects
        || host.sabotage_costs.size() > galactic_max_objects || host.neutralization_targets.size() > galactic_max_objects) return bad();
    for (const auto* list : {&host.activated_sabotage_abilities, &host.activated_neutralize_abilities})
        for (const auto& name : *list) if (name.empty() || name.size() > 4096 || name.find('\0') != std::string::npos) return bad();
    previous = 0;
    for (const auto& c : host.sabotage_costs) {
        if (!c.type || c.type <= previous || c.cost.raw() < 0) return bad();
        previous = c.type;
    }
    previous = 0;
    for (const auto& t : host.neutralization_targets) {
        if (!t.type || t.type <= previous || t.cost.raw() < 0) return bad();
        previous = t.type;
    }
    for (const auto& p : host.profiles) {
        if (p.sabotage && (p.sabotage->name.empty() || p.sabotage->name.size() > 4096 || p.sabotage->name.find('\0') != std::string::npos
            || p.sabotage->cost_factor.raw() < 0 || !setup.control || !setup.production || !setup.economy_corruption)) return bad();
        if (p.neutralize) {
            const auto& n = *p.neutralize;
            if (n.name.empty() || n.name.size() > 4096 || n.name.find('\0') != std::string::npos || !setup.control || !setup.production
                || n.actor_absence_min > n.actor_absence_max || n.target_absence_min > n.target_absence_max
                || n.minor_modifier.raw() < 0 || n.major_modifier.raw() < 0) return bad();
            for (const auto* curve : {&n.base_curve, &n.history_curve, &n.corruption_curve}) {
                if (curve->size() > galactic_max_objects) return bad();
                for (std::size_t i = 1; i < curve->size(); ++i) if ((*curve)[i - 1].x >= (*curve)[i].x) return bad();
            }
        }
    }
    // WGCH-143/176: ruined base income is an authored per-planet host input.
    if (host.ruined_income.size() > galactic_max_objects || (!host.ruined_income.empty() && !setup.control)) return bad();
    previous_planet = 0;
    for (const auto& r : host.ruined_income) {
        if (!r.planet || r.planet <= previous_planet || r.credits < math::Fixed{}
            || std::none_of(setup.planets.begin(), setup.planets.end(), [&](const auto& p) { return p.id == r.planet; })) return bad();
        previous_planet = r.planet;
    }
    return core::Result<void>::success();
}
PreparedAbility prepared_ability(const AbilityPreparation& menu, std::uint32_t index) {
    PreparedAbility result; result.preparation = menu.id; result.actor = menu.actor;
    result.planet = menu.planet; result.choice = index; result.absence = menu.absence;
    result.kind = menu.kind;
    if (menu.kind == AbilityKind::corruption) {
        if (index < menu.corruption_choices.size() && index < menu.offers.size()) { result.corruption = menu.corruption_choices[index]; result.price = menu.offers[index].price; }
        return result;
    }
    if (menu.kind == AbilityKind::planet_destruction) {
        // WGCH-115/156: no price; the supplied object and planet are the whole capability.
        return result;
    }
    if (menu.kind == AbilityKind::superweapon_hack) {
        // WGCH-66: the prepared price is the only retained fact; the weapon is never carried.
        if (index < menu.offers.size()) result.price = menu.offers[index].price;
        return result;
    }
    if (menu.kind == AbilityKind::structure_sabotage || menu.kind == AbilityKind::hero_neutralization) {
        if (index < menu.target_choices.size()) {
            const auto& choice = menu.target_choices[index];
            result.target = choice.target; result.company = choice.company; result.price = choice.price;
            result.absence = choice.absence; result.target_absence = choice.target_absence; result.valid = choice.valid;
        }
        return result;
    }
    if (menu.kind == AbilityKind::technology_theft) {
        if (index < menu.theft_choices.size()) {
            result.theft = menu.theft_choices[index]; result.price = result.theft.price; result.absence = result.theft.absence;
        }
        return result;
    }
    if (index < menu.offers.size()) {
        result.item = menu.offers[index].item; result.price = menu.offers[index].price;
        result.owned = menu.offers[index].owned;
    }
    return result;
}
void GalacticSession::Impl::refresh_ability_actor(const CensusObject& row) {
    if (!ability_host || !economy_corruption || !setup.ability_host) return;
    const auto* profile = find(setup.ability_host->profiles, row.current_type,
        [](const auto& p) { return p.type; });
    if (!profile || row.dead || row.delete_pending) { lose_ability_actor(row.id); return; }
    auto* state = find(ability_host->actors, row.id, [](const auto& a) { return a.actor; });
    if (state && state->type != row.current_type) sync_auto_spies(row.id, state->type, nullptr);
    sync_auto_spies(row.id, row.current_type, &row);
    const bool same_profile = state && state->type == row.current_type;
    if (!same_profile) {
        if (!state && ability_host->actors.size() >= galactic_max_objects) { failed = true; return; }
        put(ability_host->actors, AbilityActorState{row.id, row.current_type, profile->has_market && profile->initially_enabled, false,
            profile->theft && profile->theft->initially_enabled, false,
            profile->corruption && profile->corruption->initially_enabled, false,
            profile->hack && profile->hack->initially_enabled, false,
            profile->destruction && profile->destruction->initially_enabled, false,
            profile->sabotage && profile->sabotage->initially_enabled, false,
            profile->neutralize && profile->neutralize->initially_enabled, false,
            profile->spy && profile->spy->initially_enabled, false, {}, false}, [](const auto& a) { return a.actor; });
        touch_abilities();
    }
    const auto* before = find(economy_corruption->buyers, row.id, [](const auto& p) { return p.buyer; });
    if (!profile->has_market) {
        if (before && before->present) {
            auto value = *before; value.present = false; value.despawn_requested = false;
            put(economy_corruption->buyers, std::move(value), [](const auto& p) { return p.buyer; });
            dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
        }
        return;
    }
    if (before && same_profile && before->present && before->owner == row.owner) return;
    auto value = before && same_profile ? *before : profile->market;
    value.buyer = row.id; value.owner = row.owner; value.present = true;
    // Unrelated sparse actor facts must not erase the retained execution marker.
    if (!same_profile) { value.despawn_requested = false; value.prepared_absence = {}; }
    if (before && *before == value) return;
    if (!before && economy_corruption->buyers.size() >= galactic_max_objects) { failed = true; return; }
    put(economy_corruption->buyers, std::move(value), [](const auto& p) { return p.buyer; });
    dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
}
void GalacticSession::Impl::lose_ability_actor(EntityId id) {
    if (!ability_host || !economy_corruption) return;
    if (const auto* gone = find(ability_host->actors, id, [](const auto& row) { return row.actor; })) sync_auto_spies(id, gone->type, nullptr);
    if (id != consuming_spy) {
        const auto* state = find(ability_host->actors, id, [](const auto& row) { return row.actor; });
        if (state && !state->spy_targets.empty()) { const std::array gone{id}; detach_observer_sources(gone); }
    }
    if (auto* state = find(ability_host->actors, id, [](const auto& a) { return a.actor; })) {
        const auto index = static_cast<std::size_t>(state - ability_host->actors.data());
        ability_host->actors.erase(ability_host->actors.begin() + static_cast<std::ptrdiff_t>(index));
        touch_abilities();
    }
    if (auto* buyer = find(economy_corruption->buyers, id, [](const auto& p) { return p.buyer; })) {
        if (buyer->present || buyer->despawn_requested) {
            buyer->present = false; buyer->despawn_requested = false;
            dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
        }
    }
    if (auto* menu = find(economy_corruption->menus, id, [](const auto& p) { return p.buyer; })) {
        if (menu->active) { menu->active = false; dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true; }
    }
    // Keep prepared facts: execution performs a live actor check; loss is not repricing.
}
std::optional<HeroDefeat> GalacticSession::Impl::prepare_ability_defeat(const CensusObject& actor_value, math::Fixed absence, bool bypass) {
    HeroDefeat defeat; defeat.bypass_companions = bypass;
    const auto* original = find(setup.ability_host->profiles, actor_value.original_type, [](const auto& p) { return p.type; });
    if (!original) return {};
    defeat.id = actor_value.id; defeat.owner = actor_value.owner;
    defeat.type = defeat.respawn_type = actor_value.original_type;
    defeat.planet = actor_value.planet; defeat.named = original->named;
    defeat.structure = original->respawn_structure; defeat.seconds = absence;
    defeat.default_seconds = setup.ability_host->default_hero_absence;
    defeat.respawn_whole_team = original->respawn_whole_team;
    const auto counts = original_owned_counts.find(defeat.owner);
    if (counts != original_owned_counts.end()) {
        const auto structures = counts->second.find(defeat.structure), instances = counts->second.find(defeat.respawn_type);
        if (structures != counts->second.end()) defeat.owned_structures = static_cast<std::uint32_t>(structures->second);
        if (instances != counts->second.end()) defeat.owned_instances = static_cast<std::uint32_t>(instances->second);
    }
    if (movement_inputs) for (const auto id : {actor_value.container, actor_value.id}) {
        const auto* contents = find(movement_inputs->contents, id, [](const auto& p) { return p.container; });
        if (!contents) continue;
        const auto parent = actor_records.find(id);
        if (parent == actor_records.end()) return {};
        const auto* census_type = find(setup.census->types, parent->second.original_type, [](const auto& p) { return p.type; });
        const auto* movement_type = find(movement_inputs->types, parent->second.current_type, [](const auto& p) { return p.type; });
        if (census_type && census_type->company) defeat.parent = HeroParent::team;
        else if (movement_type && movement_type->squadron) defeat.parent = HeroParent::nested_squadron;
        else if (movement_type && movement_type->transport) defeat.parent = HeroParent::transport;
        else return {}; // named parent classification seam
        for (const auto& child : contents->children) {
            const auto current = actor_records.find(child.id);
            defeat.companions.push_back({child.id, current == actor_records.end() || current->second.dead,
                current != actor_records.end() && current->second.delete_pending});
        }
        if (bypass && movement_type && movement_type->transport) {
            std::vector<EntityId> removal_work{id}; std::set<EntityId> visited;
            while (!removal_work.empty()) {
                const auto removed = removal_work.back(); removal_work.pop_back();
                if (!visited.insert(removed).second || visited.size() > galactic_max_objects) return {};
                defeat.removed_objects.push_back(removed);
                const auto* nested = find(movement_inputs->contents, removed, [](const auto& p) { return p.container; });
                if (nested) for (const auto& child : nested->children) removal_work.push_back(child.id);
            }
        }
        break;
    }
    return defeat;
}
void GalacticSession::Impl::execute_ability(const GameCommand& command, std::uint64_t tick,
    std::vector<GalacticEvent>& events) {
    const auto* prepare = std::get_if<PrepareGalacticAbility>(&command.payload);
    const auto* execute = std::get_if<ExecutePreparedAbility>(&command.payload);
    const auto* cancel = std::get_if<CancelGalacticAbility>(&command.payload);
    const auto* old_prepare = std::get_if<PrepareBlackMarket>(&command.payload);
    const auto* old_buy = std::get_if<BuyBlackMarket>(&command.payload);
    const auto actor = prepare ? prepare->actor : execute ? execute->prepared.actor : cancel ? cancel->actor
        : old_prepare ? old_prepare->buyer : old_buy->buyer;
    GalacticEvent event{tick, GalacticEventKind::command_rejected, command.key.seat, actor, command.key.sequence};
    const auto reject = [&](GalacticRejectReason why) { event.reason = why; events.push_back(event); };
    const auto* issuer = seat(command.key.seat);
    if (!issuer || !issuer->active) return reject(GalacticRejectReason::inactive_seat);
    if (!ability_host || !setup.ability_host || !economy_corruption) return reject(GalacticRejectReason::unknown_value);
    const auto live = actor_records.find(actor);
    if (live == actor_records.end() || live->second.dead || live->second.delete_pending)
        return reject(GalacticRejectReason::unknown_subject);
    if (live->second.owner != issuer->player) return reject(GalacticRejectReason::not_authorized);
    const auto actor_value = live->second; // removal cannot invalidate this copied input
    const auto* profile = find(setup.ability_host->profiles, actor_value.current_type, [](const auto& p) { return p.type; });
    const auto early_kind = prepare ? prepare->kind : execute ? execute->prepared.kind : AbilityKind::black_market;
    // A cancellation needs only the retained preparation: a supplied transport (WGCH-155) may own no profile.
    if (!profile && !cancel && early_kind != AbilityKind::planet_destruction) return reject(GalacticRejectReason::unknown_value);
    auto* retained = find(ability_host->preparations, actor, [](const auto& p) { return p.actor; });
    const auto changed = [&] { touch_abilities(); };
    const auto result_full = ability_host->results.size() >= galactic_max_objects
        && !find(ability_host->results, actor, [](const auto& p) { return p.actor; });
    if (ability_host->next_request == std::numeric_limits<std::uint64_t>::max() || result_full)
        return reject(GalacticRejectReason::resource_limit);
    if (cancel) {
        if (!retained || retained->id != cancel->preparation || !retained->open)
            return reject(GalacticRejectReason::unknown_value);
        retained->open = false; changed();
        AbilityResult result{ability_host->next_request++, retained->id,
            actor, retained->planet, actor_value.owner, 0, AbilityOutcome::cancelled}; result.kind = retained->kind;
        put(ability_host->results, std::move(result), [](const auto& p) { return p.actor; });
        if (auto* menu = find(economy_corruption->menus, actor, [](const auto& p) { return p.buyer; })) menu->active = false;
        dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
        event.kind = GalacticEventKind::ability_cancelled; event.value = retained->id; events.push_back(event); return;
    }
    if (early_kind == AbilityKind::planet_destruction && (prepare || execute)) {
        // WGCH-155/156: the ORIGINAL supplied object and planet; abilities are resolved again at execution.
        if (prepare) {
            if (ability_host->preparations.size() >= galactic_max_objects && !retained) return reject(GalacticRejectReason::resource_limit);
            prepare_destruction(command, actor_value, tick, events); return;
        }
        const auto& request = execute->prepared;
        // A queued drop carries no host-issued menu: nothing priced can be forged. A named preparation must match.
        if (request.price != math::Fixed{} || request.absence != math::Fixed{} || request.choice || request.item || request.owned
            || (request.preparation && (!retained || retained->kind != AbilityKind::planet_destruction || !retained->open
                || request != prepared_ability(*retained, 0)))) return reject(GalacticRejectReason::unknown_value);
        ++ability_work.executions;
        execute_destruction(command, actor_value, request, tick, events); return;
    }
    const auto* service = find(ability_host->actors, actor, [](const auto& a) { return a.actor; });
    const auto kind = prepare ? prepare->kind : execute ? execute->prepared.kind : AbilityKind::black_market;
    if (!service || (kind == AbilityKind::corruption
        ? !profile->corruption || !service->corruption_enabled || service->corruption_cancelled
        : kind == AbilityKind::superweapon_hack
        ? !profile->hack || !service->hack_enabled || service->hack_cancelled
        : kind == AbilityKind::technology_theft
        ? !profile->theft || !service->theft_enabled || service->theft_cancelled
        : kind == AbilityKind::structure_sabotage
        ? !profile->sabotage || !service->sabotage_enabled || service->sabotage_cancelled
        : kind == AbilityKind::hero_neutralization
        ? !profile->neutralize || !service->neutralize_enabled || service->neutralize_cancelled
        : kind != AbilityKind::black_market || !profile->has_market || !service->enabled || service->cancelled))
        return reject(GalacticRejectReason::unknown_value);
    if (prepare || old_prepare) ++ability_work.preparations;
    else ++ability_work.executions;
    if (prepare && kind == AbilityKind::corruption) {
        if (actor_value.planet != prepare->planet) return reject(GalacticRejectReason::incompatible_location);
        if (ability_host->preparations.size() >= galactic_max_objects && !retained) return reject(GalacticRejectReason::resource_limit);
        prepare_corruption(command, actor_value, tick, events); return;
    }
    if (prepare && kind == AbilityKind::superweapon_hack) {
        if (actor_value.planet != prepare->planet) return reject(GalacticRejectReason::incompatible_location);
        if (ability_host->preparations.size() >= galactic_max_objects && !retained) return reject(GalacticRejectReason::resource_limit);
        prepare_hack(command, actor_value, *profile, tick, events); return;
    }
    if (prepare && kind == AbilityKind::structure_sabotage) {
        if (actor_value.planet != prepare->planet) return reject(GalacticRejectReason::incompatible_location);
        if (ability_host->preparations.size() >= galactic_max_objects && !retained) return reject(GalacticRejectReason::resource_limit);
        prepare_sabotage(command, actor_value, tick, events); return;
    }
    if (prepare && kind == AbilityKind::hero_neutralization) {
        if (actor_value.planet != prepare->planet) return reject(GalacticRejectReason::incompatible_location);
        if (ability_host->preparations.size() >= galactic_max_objects && !retained) return reject(GalacticRejectReason::resource_limit);
        prepare_neutralization(command, actor_value, tick, events); return;
    }
    if (prepare && kind == AbilityKind::technology_theft) {
        if (actor_value.planet != prepare->planet) return reject(GalacticRejectReason::incompatible_location);
        if (ability_host->preparations.size() >= galactic_max_objects && !retained) return reject(GalacticRejectReason::resource_limit);
        prepare_theft(command, actor_value, *profile, tick, events); return;
    }
    if (prepare || old_prepare) {
        const auto planet = prepare ? prepare->planet : old_prepare->planet;
        if ((prepare && prepare->kind != AbilityKind::black_market) || actor_value.planet != planet)
            return reject(GalacticRejectReason::incompatible_location);
        if (ability_host->preparations.size() >= galactic_max_objects && !retained)
            return reject(GalacticRejectReason::resource_limit);
        // WGCH-37/78: only preparation reads the current opposing-planet predicate.
        const auto target = planets.find(planet);
        if (target == planets.end()) return reject(GalacticRejectReason::unknown_subject);
        const auto owner = registry.get<session_detail::PlanetData>(target->second).owner;
        bool allied = owner == issuer->player;
        if (control) for (const auto& relation : control->relations)
            if (relation.player == issuer->player && relation.other == owner) allied |= relation.allied;
        auto* buyer = find(economy_corruption->buyers, actor, [](const auto& p) { return p.buyer; });
        if (!buyer) return reject(GalacticRejectReason::unknown_value);
        if (buyer->opposing_planet != !allied) {
            buyer->opposing_planet = !allied;
            dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
        }
        const auto before = events.size();
        execute_black_market({command.key, PrepareBlackMarket{actor, planet}}, tick, events);
        if (failed || events.size() == before || events.back().kind == GalacticEventKind::command_rejected) return;
        const auto* menu = find(economy_corruption->menus, actor, [](const auto& p) { return p.buyer; });
        if (!menu) { failed = true; return; }
        const auto request = ability_host->next_request++;
        put(ability_host->preparations, AbilityPreparation{request, actor, planet, menu->offers, menu->absence, true, AbilityKind::black_market, {}},
            [](const auto& p) { return p.actor; });
        changed(); event.kind = GalacticEventKind::ability_prepared; event.value = request; events.push_back(event); return;
    }
    if (!retained) return reject(GalacticRejectReason::unknown_value);
    const auto choice = execute ? execute->prepared.choice : old_buy->index;
    if (retained->kind != kind) return reject(GalacticRejectReason::unknown_value);
    if (kind == AbilityKind::corruption) {
        if (!execute || choice >= retained->corruption_choices.size()
            || execute->prepared != prepared_ability(*retained, choice)) return reject(GalacticRejectReason::unknown_value);
        execute_corruption(command, actor_value, execute->prepared, tick, events); return;
    }
    if (kind == AbilityKind::superweapon_hack) {
        if (!execute || choice >= retained->offers.size() || !retained->open
            || execute->prepared != prepared_ability(*retained, choice)) return reject(GalacticRejectReason::unknown_value);
        execute_hack(command, actor_value, execute->prepared, tick, events); return;
    }
    if (kind == AbilityKind::structure_sabotage || kind == AbilityKind::hero_neutralization) {
        if (!execute || choice >= retained->target_choices.size() || !retained->open
            || execute->prepared != prepared_ability(*retained, choice)) return reject(GalacticRejectReason::unknown_value);
        if (kind == AbilityKind::structure_sabotage) execute_sabotage(command, actor_value, execute->prepared, tick, events);
        else execute_neutralization(command, actor_value, execute->prepared, tick, events);
        return;
    }
    if (kind == AbilityKind::technology_theft) {
        if (!execute || choice >= retained->theft_choices.size()
            || execute->prepared != prepared_ability(*retained, choice)) return reject(GalacticRejectReason::unknown_value);
        execute_theft(command, actor_value, execute->prepared, tick, events); return;
    }
    if (choice >= retained->offers.size()) return reject(GalacticRejectReason::unknown_value);
    const auto prepared = prepared_ability(*retained, choice);
    if (execute && execute->prepared != prepared) return reject(GalacticRejectReason::unknown_value);
    if (ability_host->next_request == std::numeric_limits<std::uint64_t>::max()
        || (ability_host->results.size() >= galactic_max_objects
            && !find(ability_host->results, actor, [](const auto& p) { return p.actor; })))
        return reject(GalacticRejectReason::resource_limit);
    // WGEP-80/WGCH-78: open/owned/current allegiance are not purchase gates.
    if (profile->market.causes_despawn && !ability_consumption_handler)
        return reject(GalacticRejectReason::unknown_value);
    HeroDefeat defeat;
    if (profile->market.causes_despawn) {
        auto prepared_defeat = prepare_ability_defeat(actor_value, prepared.absence, false);
        if (!prepared_defeat) return reject(GalacticRejectReason::unknown_value);
        defeat = std::move(*prepared_defeat);
    }
    auto* menu = find(economy_corruption->menus, actor, [](const auto& p) { return p.buyer; });
    if (!menu) return reject(GalacticRejectReason::unknown_value);
    *menu = {actor, retained->planet, retained->offers, retained->absence, retained->open};
    dirty[static_cast<std::size_t>(StateSection::economy_corruption)] = true;
    const auto before = events.size();
    execute_black_market({command.key, BuyBlackMarket{actor, choice}}, tick, events);
    if (failed || events.size() == before) return;
    if (events.back().kind != GalacticEventKind::black_market_purchased) {
        put(ability_host->results, AbilityResult{ability_host->next_request++, prepared.preparation,
            actor, prepared.planet, actor_value.owner, 0, AbilityOutcome::refused}, [](const auto& p) { return p.actor; });
        changed(); return;
    }
    retained->open = false; changed();
    if (profile->market.causes_despawn) {
        auto consumed = ability_consumption_handler(defeat);
        if (!consumed) { failed = true; return; }
    }
    put(ability_host->results, AbilityResult{ability_host->next_request++, prepared.preparation,
        actor, prepared.planet, actor_value.owner, prepared.item, AbilityOutcome::purchased}, [](const auto& p) { return p.actor; });
    event.kind = GalacticEventKind::ability_settled; event.value = prepared.preparation; events.push_back(event); changed();
}
core::Result<void> GalacticSession::set_ability_lifecycle(EntityId actor, bool enabled, bool cancelled, AbilityKind kind) {
    auto& impl = *impl_;
    auto* state = impl.ability_host ? find(impl.ability_host->actors, actor, [](const auto& a) { return a.actor; }) : nullptr;
    const auto* profile = state ? find(impl.setup.ability_host->profiles, state->type, [](const auto& p) { return p.type; }) : nullptr;
    if (!profile || (kind == AbilityKind::black_market ? !profile->has_market : kind == AbilityKind::technology_theft ? !profile->theft
        : kind == AbilityKind::superweapon_hack ? !profile->hack : kind == AbilityKind::planet_destruction ? !profile->destruction
        : kind == AbilityKind::system_spy ? !profile->spy
        : kind == AbilityKind::structure_sabotage ? !profile->sabotage : kind == AbilityKind::hero_neutralization ? !profile->neutralize
        : kind != AbilityKind::corruption || !profile->corruption))
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command, "ability lifecycle requires a live bound kind"));
    if (!impl.native_replaying) {
        std::vector<std::uint8_t> bytes; detail::put_u8(bytes, 0); detail::put_u64(bytes, actor); detail::put_u8(bytes, enabled ? 1 : 0); detail::put_u8(bytes, cancelled ? 1 : 0); detail::put_u8(bytes, static_cast<std::uint8_t>(kind));
        auto recorded = append_native_effect(NativeEffectKind::ability_lifecycle, std::move(bytes));
        if (!recorded) return recorded;
    }
    if (kind == AbilityKind::black_market) { state->enabled = enabled; state->cancelled = cancelled; }
    else if (kind == AbilityKind::system_spy) { state->spy_enabled = enabled; state->spy_cancelled = cancelled; }
    else if (kind == AbilityKind::technology_theft) { state->theft_enabled = enabled; state->theft_cancelled = cancelled; }
    else if (kind == AbilityKind::superweapon_hack) { state->hack_enabled = enabled; state->hack_cancelled = cancelled; }
    else if (kind == AbilityKind::planet_destruction) { state->destruction_enabled = enabled; state->destruction_cancelled = cancelled; }
    else if (kind == AbilityKind::structure_sabotage) { state->sabotage_enabled = enabled; state->sabotage_cancelled = cancelled; }
    else if (kind == AbilityKind::hero_neutralization) { state->neutralize_enabled = enabled; state->neutralize_cancelled = cancelled; }
    else { state->corruption_enabled = enabled; state->corruption_cancelled = cancelled; }
    impl.touch_abilities();
    return core::Result<void>::success();
}
GalacticSession::AbilityWork GalacticSession::ability_work() const noexcept { return impl_->ability_work; }
const GalacticAbilityState* GalacticSession::ability_state() const noexcept { return impl_->ability_host ? &*impl_->ability_host : nullptr; }
void GalacticSession::set_ground_ability_story_handler(GroundAbilityStoryHandler handler) { impl_->ground_ability_story_handler = std::move(handler); }
void GalacticSession::set_ability_consumption_handler(AbilityConsumptionHandler handler) { impl_->ability_consumption_handler = std::move(handler); }
} // namespace eawr::sim::galactic
