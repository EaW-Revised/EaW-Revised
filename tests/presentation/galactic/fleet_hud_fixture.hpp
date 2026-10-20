#pragma once

#include "eawr/sim/galactic/fleet_intelligence.hpp"
#include <map>
#include <string_view>

namespace eawr::viewer::galactic {
[[nodiscard]] inline bool fleet_hud_fixture(std::string_view name) {
    return name == "fleet-presence" || name == "fleet-presence-plain" || name == "fleet-count" || name == "fleet-contents" || name == "fleet-heroes"
        || name == "fleet-transit" || name.starts_with("fleet-drag") || name == "fleet-slot"
        || name.starts_with("fleet-route-") || name.starts_with("ground-") || name.starts_with("destruction-");
}
// Explicit synthetic provider inputs; never a stock-campaign fallback. Enemy
// hero/stealth payloads remain populated even in the count-only fixture.
inline void configure_fleet_hud_fixture(sim::galactic::GalacticSetup& setup,
    sim::galactic::PlayerId observer_player, std::string_view fixture,
    std::vector<sim::galactic::FleetDisplayType>& traits,
    std::vector<sim::galactic::FleetDisplayVisibility>& visibility,
    std::map<sim::galactic::TypeId, std::string>& names) {
    namespace gc = sim::galactic;
    const auto whole = [](std::int64_t value) { return sim::math::Fixed::from_raw(value * sim::math::Fixed::scale); };
    const gc::PlayerId enemy = observer_player == 1 ? 2 : 1;
    setup.rules.maximum_fleet_movement_distance = whole(10000); // explicit synthetic graph input
    setup.fleet_control.emplace();
    setup.fleet_control->types = {{1000,false,true,false},{1001,false,true,false},
        {1002,false,true,false},{1003,false,true,false}};
    setup.fleets = {{601,observer_player,101,{{7001,1000},{7002,1000},{7003,1003}},0},
        {602,observer_player,101,{{7004,1000}},1}, {603,enemy,101,{{7005,1000},{7006,1001},{7007,1000},{7008,1003}},2},
        {604,enemy,101,{{7009,1002}},0}};
    if (fixture == "fleet-drag-enemy") setup.fleets[3].orbit = 102;
    // WGUI-175: another owner's aware fleet at the focused planet puts its orbit panels in the enemy layout
    // (enemy panel 1, local slots 0 and 2 together in panel 3). The drag fixtures drive the friendly layout of the
    // local slots, so their other-owner fleets wait at planet 103 (undiscovered in these fixtures, so invisible);
    // fleet-slot keeps the enemy layout covered.
    if ((fixture.starts_with("fleet-drag") && fixture != "fleet-drag-enemy") || fixture.starts_with("fleet-route-"))
        setup.fleets[2].orbit = setup.fleets[3].orbit = 103;
    if (fixture.starts_with("destruction-")) setup.fleets[0].members = {{7001,1000}};
    // WGUI-17: an attached superweapon (either side's) grants counts under a
    // presence-only mask; the plain variant drops both Death Stars so presence stays bare.
    if (fixture == "fleet-presence-plain") {
        setup.fleets[0].members = {{7001,1000},{7002,1000}};
        setup.fleets[2].members = {{7005,1000},{7006,1001},{7007,1000}};
    }
    // WGEP-40: a purchased orbit unit is a fleet of its own at slot 0, never
    // merged into the slot's existing fleet. 7010 shares a type with fleet 601's
    // first group; 7011 is a type only this fleet holds.
    // fleet-drag-slot is the same orbit with the other owner away: the friendly layout the drag cases drive.
    if (fixture == "fleet-slot" || fixture == "fleet-drag-slot") setup.fleets.push_back({605,observer_player,101,{{7010,1000},{7011,1002}},0});
    // A singleton before a count-two stack lets the drag test free an earlier
    // cell, then split the stack without giving its remainder an explicit index.
    if (fixture == "fleet-drag-split")
        setup.fleets[0].members = {{7011,1002},{7001,1000},{7002,1000},{7003,1003}};
    if (fixture == "fleet-drag-autostack") {
        setup.fleets[0].members = {{7001,1000},{7002,1000},{7003,1002}};
        setup.fleets[1].members = {{7010,1000},{7011,1000},{7012,1000},{7013,1000},{7014,1000},{7015,1002}};
    }
    // #3188: fleet 601 is one loaded transport (troops carried, no ship of its own), the shape of a ground
    // force in orbit; its fleet qualifies for an explicit destination slot only through the cargo (WGM-83).
    const bool loaded = fixture == "fleet-drag-load";
    if (loaded) {
        setup.fleet_control->types.push_back({1004,true,true,false});
        setup.fleets[0].members = {{7001,1004}};
    }
    if (fixture == "fleet-transit" || fixture.starts_with("fleet-drag") || fixture == "fleet-slot"
        || fixture.starts_with("fleet-route-")) {
        // Explicit movement inputs. The runtime fixture submits a real move
        // after startup; tick-zero setups correctly reject seeded transit.
        setup.movement.emplace();
        auto& movement = *setup.movement;
        movement.mode_modifier = whole(1);
        for (const auto& type : setup.fleet_control->types)
            movement.types.push_back({type.type, whole(2), whole(100), 0, true, false, false, false, type.transport});
        if (loaded) movement.contents.push_back({7001, {{7002,1000},{7003,1000}}});
        for (const auto& player : setup.players) {
            movement.players.push_back({player.player_id, whole(1), whole(0), false});
            for (const auto& planet : setup.planets) movement.access.push_back({player.player_id, planet.id, true, false});
        }
        for (const auto& planet : setup.planets) movement.planets.push_back({planet.id, whole(1), false});
        if (fixture.starts_with("fleet-route-")) {
            // Explicit graph witnesses: a bent two-hop path, no proximity shortcut.
            setup.rules.maximum_fleet_movement_distance = whole(0);
            setup.routes = {{201,101,104,whole(1)}, {202,104,102,whole(1)}};
            if (fixture == "fleet-route-blocked")
                for (auto& row : movement.access) if (row.planet == 104) row.potential_conflict = true;
        }
    }
    setup.observer.emplace();
    auto& observer = *setup.observer;
    observer.local_player = observer_player;
    observer.reveal_delay_seconds = whole(1);
    observer.types = {{1000,false,false,false},{1001,false,true,false},
        {1002,true,false,false},{1003,false,false,true}};
    if (loaded) observer.types.push_back({1004,false,false,false});
    gc::IntelligenceMask mask = gc::bit(gc::Intelligence::fleet_presence);
    if (fixture != "fleet-presence" && fixture != "fleet-presence-plain") mask |= gc::bit(gc::Intelligence::fleet_count);
    if (fixture == "fleet-contents" || fixture == "fleet-heroes") mask |= gc::bit(gc::Intelligence::fleet_contents);
    if (fixture == "fleet-heroes") mask |= gc::bit(gc::Intelligence::force_sensitive);
    for (const auto& player : setup.players) observer.players.push_back({player.player_id,false,false,mask,1,{}});
    for (const auto& planet : setup.planets) {
        gc::ObserverPlanet actual; actual.planet = planet.id;
        actual.allegiance = actual.target_owner = actual.space_owner = actual.ground_owner = enemy;
        actual.current_owner = actual.current_controller = enemy;
        observer.planets.push_back(actual);
        visibility.push_back({planet.id, planet.id != 103});
    }
    for (const auto& player : setup.players) for (const auto& planet : setup.planets)
        observer.records.push_back({player.player_id,planet.id,planet.id != 103,0,0,mask,{}});
    traits = {{1000,false,false,false,false,whole(10),-1},
        {1001,true,true,true,false,whole(20),1}, {1002,false,false,false,false,whole(10),2},
        {1003,false,false,false,false,whole(1),3}};
    names = {{1000,"Star_Destroyer"},{1001,"Darth_Vader"},{1002,"Tartan_Patrol_Cruiser"},{1003,"Death_Star"}};
    if (fixture == "fleet-drag-autostack") {
        names[1000] = "X-Wing";
        names[1002] = "Y-Wing";
    }
    if (loaded) {
        traits.push_back({1004,false,false,false,true,whole(10),-1});
        names.emplace(1004, "Gallofree_Transport");
    }
}
} // namespace eawr::viewer::galactic
