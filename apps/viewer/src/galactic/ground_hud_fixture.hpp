#pragma once
#include "fleet_hud_fixture.hpp"
#include "hud_clock_fixture.hpp"

namespace eawr::viewer::galactic {
inline bool ground_fixture(std::string_view name) { return name.starts_with("ground-"); }
inline void configure_ground_fixture(sim::galactic::GalacticSetup& setup,
    sim::galactic::PlayerId local, std::string_view name,
    std::vector<sim::galactic::FleetDisplayType>& traits,
    std::vector<sim::galactic::FleetDisplayVisibility>& visibility,
    std::map<sim::galactic::TypeId,std::string>& names) {
    namespace g = sim::galactic;
    // GHUD-S68: the structure-row fixtures also carry a production table with the planet's built land
    // structures. The production helper resets the fleet and observer tables, so it runs first.
    const bool structures = name.starts_with("ground-structures");
    const bool sale_units = name == "ground-unit-sale";
    if (structures || sale_units) configure_synthetic_hud_production(setup,local);
    configure_fleet_hud_fixture(setup,local,"fleet-drag",traits,visibility,names);
    const auto enemy = local == 1 ? 2U : 1U;
    // The hidden-structures variant is the opponent's planet: a local owner always knows its structures.
    const auto owner = name == "ground-count" || name == "ground-foreign" || name == "ground-structures-hidden" ? enemy : local;
    std::erase_if(setup.fleets, [local](const auto& row) { return row.owner != local; });
    setup.fleet_control->types[0].transport = true;
    names[1000] = "Stormtrooper_Team";
    setup.control.emplace(); auto& control = *setup.control;
    control.change_seconds = sim::math::Fixed::from_raw(60 * sim::math::Fixed::scale);
    control.max_ground_forces = 10; // Explicit synthetic provider constant.
    for (const auto& player : setup.players) {
        control.players.push_back({player.player_id,player.player_id == setup.players.back().player_id,
            player.player_id != setup.players.back().player_id,player.faction_id});
        for (const auto& other : setup.players)
            control.relations.push_back({player.player_id,other.player_id,player.player_id == other.player_id});
    }
    for (const auto& type : setup.fleet_control->types) control.types.push_back({type.type,0,false,true});
    for (auto& planet : setup.planets) {
        g::ControlPlanet p; p.planet = planet.id; p.type = planet.id;
        p.surface_accessible = name != "ground-inaccessible";
        if (planet.id == 101) { planet.owner = owner; p.landed = {{8001,1000,owner},{8002,1000,owner},{8003,1000,owner}}; }
        control.planets.push_back(p);
    }
    for (const auto& route : setup.routes) control.routes.push_back({route.id,{}});
    for (auto& player : setup.observer->players)
        if (name == "ground-foreign" && player.player == local) player.allies.push_back(enemy);
    for (auto& planet : setup.observer->planets) if (planet.planet == 101) {
        planet.allegiance = planet.target_owner = planet.ground_owner = planet.space_owner = owner;
        planet.current_owner = planet.current_controller = owner;
        planet.landed_transports = {{8001,owner,false,false},{8002,owner,false,false},{8003,owner,true,false}};
    }
    if (structures) {
        // Five land positions, three built: two of the planet owner's, one owned by the other side.
        // Types 1004 and 1005 are structure types of the census (population 0).
        const auto whole = [](std::int64_t value) { return sim::math::Fixed::from_raw(value * sim::math::Fixed::scale); };
        setup.census->types.push_back({1005, whole(0), 0});
        names[1004] = "E_Ground_Barracks"; names[1005] = "E_Ground_Turbolaser_Tower";
        for (auto& row : setup.production->planets) if (row.planet == 101) {
            auto tower = row.offers.back(); tower.type = 1005; tower.sellable = false;
            row.offers.push_back(tower); // Explicit synthetic never-sale provider for the owned tower.
            row.ground_slots = 5;
            row.target_owner = row.starbase_owner = row.ground_occupier = owner;
            const auto structure = [&](sim::EntityId id, g::TypeId type, g::PlayerId who) {
                g::ProductionObject object;
                object.id = id; object.type = type; object.owner = who;
                object.domain = g::ProductionDomain::ground; object.kind = g::ProductionKind::structure;
                return object;
            };
            row.objects = {structure(9101, 1004, owner), structure(9102, 1005, owner), structure(9103, 1004, owner == local ? enemy : local)};
        }
    }
    if (sale_units) {
        const auto whole = [](std::int64_t value) { return sim::math::Fixed::from_raw(value * sim::math::Fixed::scale); };
        for (auto& type : setup.census->types) type.authored_price = whole(101);
        setup.census->types[4].named_hero = true; // synthetic 1003 (hero cell)
        for (const auto& row : control.planets) for (const auto& unit : row.landed)
            setup.census->objects.push_back({unit.id, unit.type, unit.owner, row.planet, row.planet,
                false, false, unit.type, 0, g::ActorCombatRole::transport, {}, {}, whole(151)});
        for (const auto& fleet : setup.fleets) for (const auto& member : fleet.members)
            setup.census->objects.push_back({member.id, member.type, fleet.owner, fleet.orbit, fleet.id,
                false, false, member.type, 0, g::ActorCombatRole::ordinary, {}, {}, whole(151)});
        std::sort(setup.census->objects.begin(), setup.census->objects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        for (auto& row : setup.production->planets) row.ground_occupier = row.target_owner;
    }
    const auto mask = g::bit(g::Intelligence::ground_count) | g::bit(g::Intelligence::ownership)
        | g::bit(g::Intelligence::fleet_presence) | g::bit(g::Intelligence::fleet_count)
        | (name == "ground-count" ? 0U : g::bit(g::Intelligence::ground_contents))
        | (structures && name != "ground-structures-hidden" ? g::bit(g::Intelligence::structures) : 0U);
    for (auto& player : setup.observer->players) player.default_intelligence = mask;
    for (auto& row : setup.observer->records) row.mask = mask;
}
} // namespace eawr::viewer::galactic
