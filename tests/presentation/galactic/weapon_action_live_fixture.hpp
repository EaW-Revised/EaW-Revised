#pragma once

#include "fleet_hud_fixture.hpp"
#include "galactic/galaxy_view.hpp"
#include "eawr/sim/galactic/ability_host.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::viewer::galactic {
// WGCH-57/66/109..112/139/148: a LIVE campaign hack menu over the synthetic galaxy. Unlike the copied snapshot
// fixtures, the host owns the menu: the runtime prepares it through a real seat command after startup and the HUD
// queues the host's typed capability. Synthetic ids and prices deliberately differ from stock; this is an explicit
// provider input, never a campaign fallback.
inline constexpr sim::EntityId live_hack_actor = 7004;
inline constexpr sim::galactic::PlanetId live_hack_planet = 101;
inline constexpr sim::galactic::TypeId live_hack_required_type = 1002;
// WGCH-20..30/96: "sabotage-live" and "neutralization-live" reuse the same live host over the same synthetic galaxy.
// "<name>-entry" is the same galaxy with NO prepare hook: the run starts a hero ability by real fleet input (#4350).
[[nodiscard]] inline bool weapon_action_entry_fixture(std::string_view name) {
    return name == "sabotage-entry" || name == "neutralization-entry" || name == "corruption-travel";
}
[[nodiscard]] inline bool weapon_action_live_fixture(std::string_view name) {
    return weapon_action_entry_fixture(name) || name == "hack-live" || name == "destruction-live" || name == "destruction-live-contested"
        || name == "sabotage-live" || name == "neutralization-live";
}
[[nodiscard]] inline sim::galactic::AbilityKind weapon_action_live_kind(std::string_view name) {
    return name == "corruption-travel" ? sim::galactic::AbilityKind::corruption
        : name.starts_with("destruction-live") ? sim::galactic::AbilityKind::planet_destruction
        : name == "sabotage-live" || name == "sabotage-entry" ? sim::galactic::AbilityKind::structure_sabotage
        : name == "neutralization-live" || name == "neutralization-entry" ? sim::galactic::AbilityKind::hero_neutralization
        : sim::galactic::AbilityKind::superweapon_hack;
}
inline constexpr sim::EntityId live_sabotage_ground = 9101, live_sabotage_space = 9102;

// Run after configure_synthetic_hud_clock: the economy rows and credits come from there.
inline void configure_weapon_action_live_fixture(sim::galactic::GalacticSetup& setup,
    sim::galactic::PlayerId local, std::vector<sim::galactic::FleetDisplayType>& traits,
    std::vector<sim::galactic::FleetDisplayVisibility>& visibility, std::map<sim::galactic::TypeId, std::string>& names, std::string_view name = "hack-live") {
    namespace gc = sim::galactic;
    const auto whole = [](std::int64_t value) { return sim::math::Fixed::from_raw(value * sim::math::Fixed::scale); };
    // Observer actor 7004 (fleet 602) and the enemy weapon 7009 (fleet 604) share planet 101.
    configure_fleet_hud_fixture(setup, local, "fleet-count", traits, visibility, names);
    const bool travel = name == "corruption-travel";
    const bool entry = weapon_action_entry_fixture(name);
    const bool destruction = name.starts_with("destruction-live");
    const bool sabotage = name == "sabotage-live" || name == "sabotage-entry";
    const bool neutralize = name == "neutralization-live" || name == "neutralization-entry";
    if (travel) {
        for (auto& planet : setup.planets) if (planet.id == 102) planet.owner = local == 1 ? 2 : 1;
    }
    if (destruction || sabotage || (entry && neutralize)) {
        const gc::PlayerId enemy = local == 1 ? 2 : 1;
        for (auto& planet : setup.planets) if (planet.id == live_hack_planet) planet.owner = enemy;
        if (name == "destruction-live")
            std::erase_if(setup.fleets, [local](const auto& fleet) { return fleet.owner != local; });
    }
    constexpr gc::TypeId planet_type = 3000;
    auto& census = setup.census.emplace();
    census.types = {{1000, whole(1), 0}, {1001, whole(0), 0}, {1002, whole(0), 0}, {1003, whole(1), 0}, {2001, {}, 0}, {2002, {}, 0}, {planet_type, {}, 0}};
    for (const auto& player : setup.players) census.players.push_back({player.player_id, 50, false, {}});
    for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members)
        census.objects.push_back({member.id, member.type, fleet.owner, fleet.orbit, fleet.id, false, false, member.type, 0,
            gc::ActorCombatRole::ordinary});
    std::sort(census.objects.begin(), census.objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    auto& production = setup.production.emplace();
    auto& economy = setup.economy_corruption.emplace();
    economy.factor_min = economy.factor_max = whole(1);
    auto& control = setup.control.emplace();
    control.change_seconds = whole(10000); control.max_ground_forces = 10;
    for (const auto& player : setup.players) {
        const bool neutral = player.player_id == synthetic_neutral_owner;
        // The policy rows follow the seat roster exactly (WGEP-28/50): humans use the cap, others pay as they go.
        const bool human = std::any_of(setup.seats.begin(), setup.seats.end(), [&](const auto& seat) {
            return seat.player == player.player_id && seat.kind == sim::SeatKind::human; });
        gc::ProductionPlayer row; row.player = player.player_id; row.human = human; row.pay_as_you_go = !human;
        row.uses_population_cap = human; row.population_capacity = whole(50); production.players.push_back(row);
        gc::CorruptionEconomyPlayer account; account.player = player.player_id; account.faction = player.player_id * 10;
        account.human = human; account.playable = !neutral;
        economy.players.push_back(account);
        control.players.push_back({player.player_id, neutral, !neutral, player.faction_id});
        for (const auto& other : setup.players) control.relations.push_back({player.player_id, other.player_id, player.player_id == other.player_id});
    }
    economy.items = {{91, 10, 2, 2, whole(100), whole(100), whole(1)}};
    for (const std::uint32_t type : {1000U, 1001U, 1002U, 1003U}) control.types.push_back({type, 0, false, false});
    for (const auto& route : setup.routes) control.routes.push_back({route.id, {}});
    production.maximum_human_queue = 5;
    for (const auto& planet : setup.planets) {
        gc::PlanetProduction row; row.planet = planet.id; row.target_owner = planet.owner; row.maximum_space_level = 5;
        production.planets.push_back(row);
        gc::ControlPlanet surface; surface.planet = planet.id; surface.type = planet_type; surface.retains_residual = true;
        surface.surface_accessible = true; surface.ground_base = {planet.owner, 1}; surface.starbase = {planet.owner, 1};
        control.planets.push_back(surface);
        gc::CorruptionEconomyPlanet economic; economic.planet = planet.id; economy.planets.push_back(economic);
    }
    if (sabotage) {
        // Two special structures on the enemy planet; the planet is corrupted so the sabotage route is open (WGCH-77).
        const gc::PlayerId enemy_owner = local == 1 ? 2 : 1;
        for (auto& row : production.planets) if (row.planet == live_hack_planet)
            row.objects = {{live_sabotage_ground, 2001, enemy_owner, {}, gc::ProductionDomain::ground, gc::ProductionKind::structure},
                {live_sabotage_space, 2002, enemy_owner, {}, gc::ProductionDomain::space, gc::ProductionKind::structure}};
        for (auto& row : economy.planets) if (row.planet == live_hack_planet) row.corruption = gc::corruption_fixture::Type::black_market;
    }
    auto& host = setup.ability_host.emplace();
    host.default_hero_absence = whole(360);
    gc::GalacticAbilityProfile profile; profile.type = 1000; profile.has_market = false;
    if (travel) {
        profile.named = true; profile.corruption = gc::CorruptionAbilityProfile{"SYNTHETIC_CORRUPTION", true};
        profile.ground_activated = {gc::AbilityKind::corruption};
        host.activated_corruption_abilities = {"SYNTHETIC_CORRUPTION"};
        for (const auto& planet : setup.planets) {
            gc::CorruptionPlanet row; row.planet = planet.id; row.type = planet_type; row.retains_influence = true;
            row.slots[0].type = gc::corruption_fixture::Type::piracy; row.slots[0].price = whole(100);
            host.corruption_planets.push_back(row);
        }
        for (const auto& player : setup.players) { gc::CorruptionPlayer row; row.player = player.player_id; host.corruption_players.push_back(row); }
        auto& movement = setup.movement.emplace(); movement.mode_modifier = whole(1);
        for (const auto& type : setup.fleet_control->types)
            movement.types.push_back({type.type, whole(2), whole(5), 0, true, false, false, false, false});
        for (const auto& player : setup.players) {
            movement.players.push_back({player.player_id, whole(1), {}, false});
            for (const auto& planet : setup.planets) movement.access.push_back({player.player_id, planet.id, true, false});
        }
        for (const auto& planet : setup.planets) movement.planets.push_back({planet.id, whole(1), false});
    } else if (sabotage) {
        profile.named = true;
        profile.sabotage = gc::SabotageAbilityProfile{"SYNTHETIC_SABOTAGE", true, whole(2)};
        host.activated_sabotage_abilities = {"SYNTHETIC_SABOTAGE"};
        host.sabotage_costs = {{2001, whole(100)}, {2002, whole(60)}};
    } else if (neutralize) {
        profile.named = true;
        auto& ability = profile.neutralize.emplace(); ability.name = "SYNTHETIC_NEUTRALIZE"; ability.initially_enabled = true;
        ability.minor_modifier = ability.major_modifier = whole(1); ability.can_minor = ability.can_major = true;
        host.activated_neutralize_abilities = {"SYNTHETIC_NEUTRALIZE"};
        host.neutralization_targets = {{1002, whole(20), true, true}};
    } else if (destruction) {
        profile.destruction = gc::DestructionAbilityProfile{"SYNTHETIC_PLANET_RUIN", true};
        host.activated_destruction_abilities = {"SYNTHETIC_PLANET_RUIN"};
    } else profile.hack = gc::HackAbilityProfile{"HACK", true};
    host.profiles.push_back(profile);
    if (neutralize) {
        // The enemy fleet's member is the victim hero; its respawn facts come from a lifecycle-only profile.
        gc::GalacticAbilityProfile victim; victim.type = 1002; victim.has_market = false; victim.named = true; victim.lifecycle_only = true;
        host.profiles.push_back(victim);
    }
    host.activated_hack_abilities = {"HACK"};
    // A hackable superweapon makes a planet ineligible for neutralization (WGCH-20): none is authored there.
    host.hack_required_type = neutralize ? 0 : live_hack_required_type;
    host.hack_cost = whole(250);
}
} // namespace eawr::viewer::galactic
