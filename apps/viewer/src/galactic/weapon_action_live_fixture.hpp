#pragma once

#include "fleet_hud_fixture.hpp"
#include "galaxy_view.hpp"
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
[[nodiscard]] inline bool weapon_action_live_fixture(std::string_view name) { return name == "hack-live"; }

// Run after configure_synthetic_hud_clock: the economy rows and credits come from there.
inline void configure_weapon_action_live_fixture(sim::galactic::GalacticSetup& setup,
    sim::galactic::PlayerId local, std::vector<sim::galactic::FleetDisplayType>& traits,
    std::vector<sim::galactic::FleetDisplayVisibility>& visibility, std::map<sim::galactic::TypeId, std::string>& names) {
    namespace gc = sim::galactic;
    const auto whole = [](std::int64_t value) { return sim::math::Fixed::from_raw(value * sim::math::Fixed::scale); };
    // Observer actor 7004 (fleet 602) and the enemy weapon 7009 (fleet 604) share planet 101.
    configure_fleet_hud_fixture(setup, local, "fleet-count", traits, visibility, names);
    constexpr gc::TypeId planet_type = 3000;
    auto& census = setup.census.emplace();
    census.types = {{1000, whole(1), 0}, {1001, whole(0), 0}, {1002, whole(0), 0}, {1003, whole(1), 0}, {planet_type, {}, 0}};
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
    auto& host = setup.ability_host.emplace();
    host.default_hero_absence = whole(360);
    gc::GalacticAbilityProfile profile; profile.type = 1000; profile.has_market = false;
    profile.hack = gc::HackAbilityProfile{"HACK", true};
    host.profiles.push_back(profile);
    host.activated_hack_abilities = {"HACK"};
    host.hack_required_type = live_hack_required_type;
    host.hack_cost = whole(250);
}
} // namespace eawr::viewer::galactic
