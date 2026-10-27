#include "skirmish_start_support.hpp"
#include "eawr/core/result.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/free_space.hpp"
#include "eawr/units/unit_tables_common.hpp"
#include "eawr/units/unit_tables_combat.hpp"
#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace skirmish_start_test_support {
bool close_to(const Vec3& value, const double x, const double y) {
    const auto off = [](const Fixed have, const double want) {
        const double got = static_cast<double>(have.raw()) / static_cast<double>(Fixed::scale);
        return got - want < 0.01 && want - got < 0.01;
    };
    return off(value.x, x) && off(value.y, y) && value.z == Fixed{};
}

eawr::units::CollisionBounds bounds(const std::int64_t half_x, const std::int64_t half_y) {
    return {Vec3{whole(-half_x), whole(-half_y), whole(-1)}, Vec3{whole(half_x), whole(half_y), whole(1)}};
}

// #597 (space-movement PL-01 to PL-07): the free-space search on invented boxes.
void synthetic_placement() {
    // PL-03: with nothing in the way the point itself is free; a taken point moves the object onto
    // the first ring (1.2 times the larger side out) at -45 degrees, and the bearings go
    // counter-clockwise from there in 22.5-degree steps.
    const skirmish::PlacementBox ship{whole(-10), whole(-20), whole(10), whole(20)};
    const skirmish::FreeSpaceSearch search{at(0, 0), ship, whole(skirmish::start_search_angle_offset_degrees)};
    auto open = skirmish::find_free_space(search, {});
    expect(open && open.value() && *open.value() == at(0, 0), "PL-03: an empty point is taken as it is");
    const std::vector<skirmish::PlacementBox> pad{{whole(-5), whole(-5), whole(5), whole(5)}};
    auto first = skirmish::find_free_space(search, pad);
    expect(first && first.value() && close_to(*first.value(), 33.941, -33.941), "PL-03: ring 1 (48 units) at -45 degrees");
    auto wall = skirmish::find_free_space({at(0, 0), ship, whole(-45), whole(100)},
        std::vector<skirmish::PlacementBox>{{whole(-200), whole(-200), whole(200), whole(200)}});
    expect(wall && !wall.value(), "PL-07: nothing free within the distance");
    // PL-05: a blocker is the axis-aligned hull of its turned box.
    auto turned = skirmish::blocker_bounds(ship, at(100, 0), whole(90));
    expect(turned && turned.value().min_x == whole(80) && turned.value().max_x == whole(120)
               && turned.value().min_y == whole(-10) && turned.value().max_y == whole(10),
           "PL-05: a 20 x 40 box turned 90 degrees blocks 40 x 20");

    eawr::units::UnitTables tables;
    auto box_ship = unit_type("Box_Ship", UnitKind::ship, "Rebel", 10);
    box_ship.collision = bounds(10, 20);
    tables.units.push_back(box_ship);
    auto dot = unit_type("Dot", UnitKind::craft, "Rebel", 1);
    dot.collision = bounds(2, 2);
    tables.units.push_back(dot);
    auto pair = unit_type("Pair", UnitKind::squadron, "Rebel", std::nullopt);
    pair.members = {{"Dot", 2, Vec3{whole(30), Fixed{}, Fixed{}}}, {"Dot", 2, Vec3{whole(-30), Fixed{}, Fixed{}}}};
    pair.team_type = "Team";
    tables.units.push_back(pair);
    auto huge = unit_type("Huge", UnitKind::ship, "Rebel", 99);
    huge.collision = bounds(500, 500);
    huge.movement.space_layer = "SuperCapital";
    tables.units.push_back(huge);
    eawr::units::ObstacleType obstacle;
    obstacle.id = "Pad";
    obstacle.space_layer = "StaticObject";
    obstacle.footprint.collision_x = whole(5);
    obstacle.footprint.collision_y = whole(5);
    tables.obstacles.push_back(obstacle);

    skirmish::Fixture fixture;
    fixture.map = "data/art/maps/test.ted";
    fixture.map_sha256 = std::string(64, 'a');
    fixture.pre_built_base = false;
    fixture.free_starting_units = false;
    fixture.slots.push_back({1, "Rebel", 0, true, {"Box_Ship", "Box_Ship", "Pair", "Huge"}});
    skirmish::StartInputs inputs;
    inputs.map = fixture.map;
    inputs.map_sha256 = fixture.map_sha256;
    inputs.tables = &tables;
    inputs.factions = {{"Rebel", true, false, false, std::nullopt}, {"Neutral", false, true, true, std::nullopt}};
    inputs.lobby_colours = {{"MP_Color_Blue", {1, 2, 3}}};
    inputs.placements.push_back(marker(1, "Team_00_Spawn_Point_Marker", at(0, 0), 0));
    inputs.placements.push_back(object(2, "Pad", "SpaceBuildable", 1, "Neutral", at(0, 0), 0));
    auto built = skirmish::build_start(fixture, inputs);
    expect(static_cast<bool>(built), "PL: the placement start builds");
    if (!built) {
        std::cerr << eawr::core::format_diagnostic(built.error()) << '\n';
        return;
    }
    const auto& units = built.value().units;
    // Entities: Box_Ship, Box_Ship, Pair, Huge, the pad, then Pair's two craft.
    expect(units.size() == 7, "PL: four companies, the pad and two craft");
    if (units.size() != 7) return;
    // The pad takes the marker: the first ship goes to ring 1 at -45 degrees; the second finds
    // -45, -22.5 and 0 degrees taken by the first and stops at 22.5.
    expect(close_to(units[0].state.position, 33.941, -33.941), "PL-03: the first ship on ring 1 at -45 degrees");
    expect(close_to(units[1].state.position, 44.346, 18.369), "PL-04: the second ship on ring 1 at 22.5 degrees");
    // PL-01: each craft is searched on its own (ring step 4.8), not put on its Squadron_Offsets slot.
    expect(close_to(units[5].state.position, 8.869, -3.674), "PL-01: the first craft on ring 2 at -22.5 degrees");
    expect(close_to(units[6].state.position, 8.869, 3.674), "PL-01: the second craft on ring 2 at 22.5 degrees");
    expect(close_to(units[2].state.position, 8.869, 0.0), "V-03: the squadron's container stands at its craft's centre");
    expect(units[5].state.rotation == units[0].state.rotation, "PL-01: the craft keep the marker's facing");
    expect(units[3].state.position == at(0, 0), "PL-02: a super capital is placed on the marker");
    expect(units[4].state.position == at(0, 0), "the pad keeps its own position");

    // PL-03: the start angle is the marker's yaw less 45 degrees (Coruscant's Rebel marker faces 336,
    // the Empire's 149), so a first ring point lies at -69 and at 104 degrees; a marker at yaw 0 gave
    // -45 above.
    for (const auto& [yaw, x, y] : {std::tuple{336, 17.202, -44.812}, std::tuple{149, -11.612, 46.574}}) {
        auto turned_inputs = inputs;
        turned_inputs.placements.front().orientation_degrees->z = whole(yaw);
        const auto faced = skirmish::build_start(fixture, turned_inputs);
        expect(faced && faced.value().units.size() == 7 && close_to(faced.value().units[0].state.position, x, y),
               "PL-03: the first ring starts at the marker's yaw less 45 degrees");
    }

    // PL-07: when nothing is free within 2500 units the company goes to the origin.
    tables.obstacles.front().footprint.collision_x = whole(4000);
    tables.obstacles.front().footprint.collision_y = whole(4000);
    inputs.placements.front().position = at(1000, 1000);
    auto walled = skirmish::build_start(fixture, inputs);
    expect(walled && walled.value().units.size() == 7 && walled.value().units[0].state.position == at(0, 0)
               && walled.value().units[3].state.position == at(1000, 1000),
           "PL-07: a ship with no free space goes to the origin; the super capital stays on the marker");
}

void synthetic_failures() {
    const auto tables = synthetic_tables();
    const auto base_fixture = synthetic_fixture();
    const auto base_inputs = synthetic_inputs(tables);
    const auto fails = [&](const skirmish::Fixture& fixture, const skirmish::StartInputs& inputs,
                           const std::string_view code, const std::string_view label) {
        const auto built = skirmish::build_start(fixture, inputs);
        expect(!built && built.error().code == code, label);
    };
    {
        auto inputs = base_inputs;
        inputs.map_sha256 = std::string(64, 'b');
        fails(base_fixture, inputs, skirmish::diagnostic_codes::fixture, "another map hash fails");
    }
    {
        auto inputs = base_inputs;
        inputs.tables = nullptr;
        fails(base_fixture, inputs, skirmish::diagnostic_codes::input, "no unit tables fails");
    }
    {
        auto inputs = base_inputs;
        inputs.placements.erase(inputs.placements.begin() + 4);
        const auto start = skirmish::build_start(base_fixture, inputs);
        expect(start && std::none_of(start.value().units.begin(), start.value().units.end(), [](const auto& unit) {
            return unit.state.owner == 2 && unit.role != skirmish::UnitRole::station;
        }), "WSS-54: a team with no spawn marker keeps its station but gets no starting fleet");
    }
    {
        auto inputs = base_inputs;
        std::erase_if(inputs.placements, [](const auto& p) { return p.type == "Team_00_Space_Station"; });
        fails(base_fixture, inputs, skirmish::diagnostic_codes::fixture, "a team without a station fails");
    }
    {
        auto inputs = base_inputs;
        inputs.placements[0].marker_for.erase(inputs.placements[0].marker_for.begin() + 1);
        fails(base_fixture, inputs, skirmish::diagnostic_codes::fixture, "no station for the faction fails");
    }
    {
        auto fixture = base_fixture;
        fixture.slots[1].fleet.push_back("Missing_Type");
        fails(fixture, base_inputs, skirmish::diagnostic_codes::fixture, "a type the tables lack fails");
    }
    {
        // SK-20: mod maps tilt markers (AaW _mp_space_kuat: 350, 350, 0). The station takes the
        // marker's whole facing, Rz(yaw) Ry(pitch) Rx(roll); a company keeps the yaw and reports the rest.
        auto inputs = base_inputs;
        inputs.placements[0].orientation_degrees = Vec3{whole(350), whole(10), whole(90)};
        inputs.placements[1].orientation_degrees->y = whole(1);
        const auto built = skirmish::build_start(base_fixture, inputs);
        expect(built.has_value(), "a tilted station or spawn marker is accepted");
        if (built) {
            const auto station = std::find_if(built.value().units.begin(), built.value().units.end(),
                [](const auto& unit) { return unit.role == skirmish::UnitRole::station && unit.record == 0; });
            // Yaw 90 then pitch 10: the +X axis points along +Y and 10 degrees down (sin 10 = 0.1736).
            bool facing = false;
            if (station != built.value().units.end()) {
                const auto matrix = eawr::sim::math::to_matrix(station->state.rotation, station->state.position);
                facing = matrix && near(matrix.value().rows[0][0], 0, 64)
                    && near(matrix.value().rows[2][0], -Fixed::scale * 1736 / 10000, Fixed::scale / 1000)
                    && !station->dropped_orientation_degrees && station->yaw_degrees == whole(90);
            }
            expect(facing, "the station takes the tilted marker's whole facing");
            const auto company = std::find_if(built.value().units.begin(), built.value().units.end(),
                [](const auto& unit) { return unit.role != skirmish::UnitRole::station && unit.record == 1; });
            expect(company != built.value().units.end() && company->state.rotation.x == Fixed{}
                    && company->state.rotation.y == Fixed{} && company->dropped_orientation_degrees
                    && company->dropped_orientation_degrees->y == whole(1),
                "a company on a tilted spawn marker keeps the yaw and reports the dropped angles");
        }
    }
    {
        auto inputs = base_inputs;
        inputs.placements[6].owner_index.reset();
        fails(base_fixture, inputs, skirmish::diagnostic_codes::fixture, "a map object without an owner index fails");
    }
    {
        auto inputs = base_inputs;
        inputs.factions[3].neutral = false;
        fails(base_fixture, inputs, skirmish::diagnostic_codes::fixture,
            "an owner index past the factions without a neutral faction fails");
    }
    {
        // Without a Neutral player, the Neutral objects and the kept decoration are deleted too.
        auto inputs = base_inputs;
        inputs.factions[3].multiplayer_player = false;
        const auto built = skirmish::build_start(base_fixture, inputs);
        expect(built && built.value().players.size() == 4 && built.value().removed.size() == 6,
               "no Neutral player: only the Pirates Hulk stays");
        if (built) {
            const auto hulk = std::find_if(built.value().units.rbegin(), built.value().units.rend(),
                [](const skirmish::StartUnit& unit) { return unit.role == skirmish::UnitRole::map_object; });
            expect(hulk != built.value().units.rend() && hulk->type == "Hulk" && hulk->state.owner == 4,
                   "the Hulk goes to the Pirates player");
        }
    }
    {
        auto inputs = base_inputs;
        inputs.lobby_colours.resize(2);
        fails(base_fixture, inputs, skirmish::diagnostic_codes::fixture, "a slot without an MP colour fails");
    }
    {
        auto fixture = base_fixture;
        fixture.slots[2].slot = 2;
        fails(fixture, base_inputs, skirmish::diagnostic_codes::fixture, "repeated lobby slots fail");
    }
    {
        auto fixture = base_fixture;
        fixture.pre_built_base = false;
        fixture.free_starting_units = false;
        const auto built = skirmish::build_start(fixture, base_inputs);
        expect(built && built.value().units.size() == 7, "no base and no free units leaves the fleets and map objects");
        if (built) {
            expect(built.value().units[0].type == "Cruiser" && built.value().units[0].role == skirmish::UnitRole::fleet,
                   "the fleet comes first without a base");
        }
    }
}

// A replay of the setup alone: header, player and unit tables, no commands.
void replay_round_trip() {
    const auto tables = synthetic_tables();
    const auto built = skirmish::build_start(synthetic_fixture(), synthetic_inputs(tables));
    if (!built) return;
    const auto& setup = built.value().setup;
    auto live = tactical::TacticalSession::create(setup);
    expect(static_cast<bool>(live), "session from the setup");
    if (!live) return;
    for (const std::uint64_t ticks : {std::uint64_t{0}, std::uint64_t{30}}) {
        const auto bytes = tactical::write_replay({setup, ticks, {}});
        std::size_t squadron_bytes = 0;
        for (const auto& squadron : setup.squadrons) squadron_bytes += 16U + 8U * squadron.members.size();
        const auto disabled = std::count_if(setup.units.begin(), setup.units.end(),
            [](const auto& unit) { return !unit.garrison_enabled; });
        const auto extension_bytes = disabled == 0 ? 0U : 8U + 8U * static_cast<std::size_t>(disabled);
        expect(bytes && bytes.value().size() == tactical::replay_header_size + extension_bytes + 24U * setup.players.size()
                                                    + 80U * setup.units.size() + squadron_bytes,
               "the replay is the header and setup tables alone");
        if (!bytes) continue;
        const auto parsed = tactical::parse_replay(bytes.value());
        expect(parsed && parsed.value().setup == setup && parsed.value().commands.empty(), "the setup round-trips");
        if (!parsed) continue;
        const auto replayed = tactical::TacticalSession::from_replay(parsed.value());
        expect(replayed && replayed.value().state_sha256() == live.value().state_sha256()
                   && replayed.value().snapshot()->sha256() == live.value().snapshot()->sha256(),
               "the replay header alone reproduces the tick-zero hashes");
    }
    auto fixture = synthetic_fixture();
    const auto inputs = synthetic_inputs(tables);
    fixture.victory_condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    fixture.slots[0].colour_index = 2;
    for (const auto roles : {std::array{false, false, false}, std::array{false, true, true},
                             std::array{true, false, false}, std::array{true, true, true}}) {
        for (std::size_t index = 0; index < roles.size(); ++index) fixture.slots[index].human = roles[index];
        const auto source = skirmish::build_start(fixture, inputs);
        expect(static_cast<bool>(source), "recorded role fixture builds");
        if (!source) continue;
        const auto recording = skirmish::recording_setup(fixture, source.value());
        const auto bytes = tactical::write_replay({recording, 30, {}});
        const auto parsed = bytes ? tactical::parse_replay(bytes.value())
            : eawr::core::Result<tactical::TacticalReplay>::failure(bytes.error());
        expect(static_cast<bool>(parsed), "live setup metadata parses");
        if (!parsed) continue;
        auto wrong_base = synthetic_fixture();
        wrong_base.slots.resize(2);
        wrong_base.map = "data/art/maps/unrelated.ted";
        const auto restored = skirmish::replay_fixture(wrong_base, inputs, parsed.value().setup);
        const auto rebuilt = restored ? skirmish::build_start(restored.value(), inputs)
            : eawr::core::Result<skirmish::SkirmishStart>::failure(restored.error());
        expect(rebuilt && rebuilt.value().setup == source.value().setup
            && skirmish::human_slots(restored.value()) == skirmish::human_slots(fixture)
            && rebuilt.value().victory_condition == source.value().victory_condition
            && rebuilt.value().players.front().colour->rgb == source.value().players.front().colour->rgb,
            "recording restores its roles, map, seed, palette, fleets and victory regardless of the base fixture");
    }
}


// TSW-01: a tactical super weapon is repositioned once from the nearest enemy-owned target, then faces the
// battlefield centre. The values are those of the debug build's rule: start distance on the line from the target
// through the object, the target's height when matched, then the authored z adjustment.
void synthetic_tactical_super_weapon() {
    eawr::units::UnitType::TacticalSuperWeapon data;
    data.start_distance = whole(8000); data.attack_distance = whole(5800);
    data.z_adjust = whole(1200); data.start_pos_match_target_z = true;
    const std::vector<Vec3> targets{Vec3{whole(0), whole(0), whole(-4300)}, Vec3{whole(30000), whole(0), whole(0)}};
    const Vec3 current{whole(0), whole(6000), whole(0)};
    const auto attacker = skirmish::tactical_super_weapon_start(data, current, targets, false);
    expect(static_cast<bool>(attacker), "TSW-01: the attacker's start computes");
    if (!attacker) return;
    expect(attacker.value().position.x == Fixed{} && attacker.value().position.y.raw() / Fixed::scale == 6502
        && attacker.value().position.z == whole(-3100),
        "TSW-01: 8000 from the nearest target along the line through the object, at the target's height plus 1200");
    expect(attacker.value().yaw_degrees == whole(-90), "TSW-01: it then faces the battlefield centre");
    const auto defender = skirmish::tactical_super_weapon_start(data, current, targets, true);
    expect(defender && defender.value().position.y.raw() / Fixed::scale == 4714 && defender.value().position.z == whole(-3100),
        "TSW-01: the defending player's object stands at the attack distance instead");
    data.start_pos_match_target_z = false;
    const auto unmatched = skirmish::tactical_super_weapon_start(data, current, targets, false);
    expect(unmatched && unmatched.value().position.z.raw() / Fixed::scale == 1560,
        "TSW-01: without the match flag the line's own height carries over, plus the adjustment");
    const auto alone = skirmish::tactical_super_weapon_start(data, current, {}, false);
    expect(alone && alone.value().position == current && alone.value().yaw_degrees == whole(-90),
        "TSW-01: with no target it stays where it is and only faces the centre");
    const auto nearer = skirmish::tactical_super_weapon_start(data, Vec3{whole(29000), whole(0), whole(0)}, targets, false);
    expect(nearer && nearer.value().position.x.raw() / Fixed::scale >= 21999 && nearer.value().position.x.raw() / Fixed::scale <= 22000,
        "TSW-01: the nearest of several targets anchors the start");
    auto marked = object(41, "Test_Planet", "SpaceProp", 3, "Neutral", at(700, -900), 90);
    marked.tactical_super_weapon_target = true; marked.layer_z_adjust = whole(-1300);
    const auto found = skirmish::tactical_super_weapon_targets({marked, object(42, "Other", "SpaceProp", 3, "Neutral", at(0, 0), 0)}, "Empire");
    expect(found && found.value() == std::vector<Vec3>{Vec3{whole(700), whole(-900), whole(0)}},
        "TSW-01: a target stands at its authored map position, its Layer_Z_Adjust not added");
    // Enemy-owned only: a nearer target of the object's own faction is skipped; a target of no mapped faction counts.
    auto friendly = object(43, "Friendly_Planet", "SpaceProp", 0, "Empire", at(100, 100), 0);
    friendly.tactical_super_weapon_target = true;
    auto unmapped = object(44, "Unmapped_Planet", "SpaceProp", 9, "", at(5000, 5000), 0);
    unmapped.tactical_super_weapon_target = true;
    const std::vector<skirmish::MapPlacement> mixed{friendly, marked, unmapped};
    const auto enemy = skirmish::tactical_super_weapon_targets(mixed, "empire");
    expect(enemy && enemy.value() == std::vector<Vec3>{Vec3{whole(700), whole(-900), whole(0)}, Vec3{whole(5000), whole(5000), whole(0)}},
        "TSW-01: a target owned by the object's own faction is skipped (case-blind), a neutral or unmapped one is not");
    const auto anyone = skirmish::tactical_super_weapon_targets(mixed, {});
    expect(anyone && anyone.value().size() == 3, "TSW-01: without an owner faction nothing is skipped");
    const Vec3 near_friendly{whole(150), whole(150), whole(0)};
    const auto anchored = skirmish::tactical_super_weapon_start(data, near_friendly, enemy.value(), false);
    // 8000 from (700, -900) along the line to (150, 150): x about -3000, y about 6190. Anchored on the friendly (100, 100) it
    // would be x = y = about 5760.
    expect(anchored && anchored.value().position.x.raw() / Fixed::scale < 0
        && anchored.value().position.y.raw() / Fixed::scale > 6100 && anchored.value().position.y.raw() / Fixed::scale < 6300,
        "TSW-01: the start anchors on the enemy target, not on the nearer friendly one");
}

// SK-05 (space-movement LZ-01, LZ-02): every created object is raised by its type's
// Layer_Z_Adjust over the point it is placed on; a squadron's team container is not.
void synthetic_heights() {
    auto tables = synthetic_tables();
    for (auto& type : tables.units) {
        if (type.id == "Station_A") type.movement.layer_z_adjust = whole(-150);
        if (type.id == "Cruiser") type.movement.layer_z_adjust = whole(-90);
        if (type.id == "Fighter") type.movement.layer_z_adjust = whole(-5);
        if (type.id == "Fighter_Squadron") type.movement.layer_z_adjust = whole(-7); // ignored: LZ-02
    }
    auto inputs = synthetic_inputs(tables);
    for (auto& placement : inputs.placements) {
        if (placement.type == "Pad") placement.layer_z_adjust = whole(-30);
        if (placement.type == "Team_00_Spawn_Point_Marker") placement.position->z = whole(12);
    }
    auto built = skirmish::build_start(synthetic_fixture(), inputs);
    expect(static_cast<bool>(built), "the start with heights builds");
    if (!built) return;
    std::map<std::string, std::vector<const skirmish::StartUnit*>> by_type;
    for (const auto& unit : built.value().units) by_type[unit.type].push_back(&unit);
    const auto heights = [&](const std::string& type) {
        std::vector<std::int64_t> z;
        for (const auto* unit : by_type[type]) z.push_back(unit->state.position.z.raw() / Fixed::scale);
        return z;
    };
    expect(heights("Station_A") == std::vector<std::int64_t>{-150, -150}, "LZ-01: the stations stand at -150");
    expect(heights("Station_B") == std::vector<std::int64_t>{0}, "LZ-01: a type without the tag stands on its marker");
    // Slots 1 and 2 spawn on markers at z 0, slot 3 on the raised one (z 12).
    expect(heights("Cruiser") == std::vector<std::int64_t>{-90, -90, -78}, "LZ-01: the cruisers over their markers");
    expect(heights("Pad") == std::vector<std::int64_t>{-30}, "LZ-01: a map object takes its placement's height");
    // A craft is placed on its own (PL-01) on the marker's plane and raised by its own height, not
    // its squadron's (-7); the container stands at the centre of its craft (V-03), so at their height.
    for (const auto& squadron : built.value().setup.squadrons) {
        const auto& units = built.value().units;
        const auto container = std::find_if(units.begin(), units.end(),
            [&](const skirmish::StartUnit& unit) { return unit.state.entity_id == squadron.container; });
        for (const auto member : squadron.members) {
            const auto found = std::find_if(units.begin(), units.end(),
                [&](const skirmish::StartUnit& unit) { return unit.state.entity_id == member; });
            const auto z = found == units.end() ? std::int64_t{} : found->state.position.z.raw() / Fixed::scale;
            expect(found != units.end() && (z == -5 || z == 12 - 5), "LZ-02: a craft stands at its own height over its marker");
            expect(container != units.end() && found != units.end() && container->state.position.z == found->state.position.z,
                   "LZ-02: the container stands at its craft's height");
        }
    }
}


} // namespace skirmish_start_test_support
