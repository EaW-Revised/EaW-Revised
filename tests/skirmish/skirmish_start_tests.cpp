// P2-04 (#67) tick-zero contracts. The synthetic placements, factions and unit
// tables are invented here. The committed m2-start replay is checked without
// the game: its header and setup alone give the pinned tick-zero hash. With
// EAWR_EAW_GAME_ROOT set, the pinned FoC fixture is also built read-only from
// the installation and must reproduce that replay byte for byte; nothing from
// the installation is written.

#include "eawr/skirmish/placement.hpp"
#include "eawr/skirmish/start.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/units/unit_tables.hpp"
#include "eawr/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace skirmish = eawr::skirmish;
namespace tactical = eawr::sim::tactical;
using eawr::units::Fixed;
using eawr::units::UnitKind;
using eawr::units::Vec3;

// The tick-zero state hash of the committed m2-start replay (FoC data, fixture seed 67).
constexpr std::string_view m2_tick_zero_state = "3cfadb5ada5c5cd1a7551ffde3f13341a290ef3efa421f234cb5c6c4e99fd7a3";
// docs/unit-data.md: the FoC fleet's unit-table identity, the replay's content identity.
constexpr std::string_view m2_content_identity = "2e2540011dfee39f1e8ef1924b6232f172170a4d51a860c383267832aecb6079";

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

Fixed whole(const std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

Vec3 at(const std::int64_t x, const std::int64_t y) { return {whole(x), whole(y), Fixed{}}; }

std::string hex(const std::array<std::uint8_t, 32>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : bytes) {
        text.push_back(digits[byte >> 4U]);
        text.push_back(digits[byte & 15U]);
    }
    return text;
}

bool near(const Fixed value, const std::int64_t raw, const std::int64_t quanta = 8) {
    const auto difference = value.raw() - raw;
    return difference >= -quanta && difference <= quanta;
}

skirmish::MapPlacement marker(const std::uint32_t record, std::string type, const Vec3 position, const std::int64_t yaw) {
    skirmish::MapPlacement placement;
    placement.record = record;
    placement.type = std::move(type);
    placement.element = "Marker";
    placement.owner_index = 3;
    placement.owner_faction = "Neutral";
    placement.position = position;
    placement.orientation_degrees = Vec3{Fixed{}, Fixed{}, whole(yaw)};
    placement.marker = true;
    return placement;
}

skirmish::MapPlacement object(const std::uint32_t record, std::string type, std::string element,
    const std::int32_t owner, std::string faction, const Vec3 position, const std::int64_t yaw) {
    skirmish::MapPlacement placement;
    placement.record = record;
    placement.type = std::move(type);
    placement.element = std::move(element);
    placement.owner_index = owner;
    placement.owner_faction = std::move(faction);
    placement.position = position;
    placement.orientation_degrees = Vec3{Fixed{}, Fixed{}, whole(yaw)};
    placement.hull = whole(20);
    return placement;
}

eawr::units::UnitType unit_type(std::string id, const UnitKind kind, std::string affiliation,
    const std::optional<std::int64_t> power) {
    eawr::units::UnitType type;
    type.id = std::move(id);
    type.kind = kind;
    type.affiliation = std::move(affiliation);
    if (power) type.ai_combat_power = whole(*power);
    return type;
}

eawr::units::UnitTables synthetic_tables() {
    eawr::units::UnitTables tables;
    auto station_a = unit_type("Station_A", UnitKind::station, "Rebel", 100);
    station_a.hull = whole(1600);
    station_a.space_fow_reveal_range = whole(2000);
    station_a.reveal = true;
    station_a.victory_relevant = true;
    eawr::units::Spawner spawner;
    spawner.starting.push_back({"Fighter_Squadron", 2, 2});
    spawner.delay_seconds = whole(10);
    spawner.reserves.push_back({"Fighter_Squadron", 2, -1});
    station_a.spawner = spawner;
    tables.units.push_back(station_a);
    tables.units.push_back(unit_type("Station_B", UnitKind::station, "Empire", 200));
    auto squadron = unit_type("Fighter_Squadron", UnitKind::squadron, "Rebel, Empire", std::nullopt);
    for (int member = 0; member < 3; ++member) squadron.members.push_back({"Fighter", 4, std::nullopt});
    squadron.team_type = "Team";
    squadron.team_reveal_range = whole(800); // #271: the squadron reveals through its container
    tables.units.push_back(squadron);
    auto cruiser = unit_type("Cruiser", UnitKind::ship, "Rebel, Empire", 50);
    cruiser.space_fow_reveal_range = whole(1200);
    cruiser.reveal = true;
    tables.units.push_back(cruiser);
    auto fighter = unit_type("Fighter", UnitKind::craft, "Rebel, Empire", 7);
    fighter.space_fow_reveal_range = whole(500); // authored, but no REVEAL: no sensor (#271)
    tables.units.push_back(fighter);
    return tables;
}

skirmish::Fixture synthetic_fixture() {
    skirmish::Fixture fixture;
    fixture.map = "data/art/maps/test.ted";
    fixture.map_sha256 = std::string(64, 'a');
    fixture.slots.push_back({1, "Rebel", 0, true, {"Cruiser"}});
    fixture.slots.push_back({2, "Empire", 1, false, {}});
    fixture.slots.push_back({3, "Rebel", 0, false, {"Cruiser"}});
    fixture.seed = 5;
    return fixture;
}

skirmish::StartInputs synthetic_inputs(const eawr::units::UnitTables& tables) {
    skirmish::StartInputs inputs;
    inputs.map = "data/art/maps/test.ted";
    inputs.map_sha256 = std::string(64, 'a');
    inputs.tables = &tables;
    // Retail ownership: Rebel and Empire are playable, Pirates and Neutral get a skirmish
    // player, Wildlife (index 4) is non-playable without one.
    inputs.factions = {{"Rebel", true, false, false, std::nullopt}, {"Empire", true, false, false, std::nullopt},
        {"Pirates", false, true, false, std::nullopt}, {"Neutral", false, true, true, std::nullopt}, {"Wildlife", false, false, false, std::nullopt}};
    inputs.faction_forces = {{"Rebel", {"Fighter_Squadron"}}, {"Empire", {"Fighter_Squadron", "Cruiser"}}};
    inputs.lobby_colours = {{"MP_Color_Blue", {1, 2, 3}}, {"MP_Color_Red", {4, 5, 6}}, {"MP_Color_Green", {7, 8, 9}}};
    auto station = marker(0, "Team_00_Space_Station", at(100, 200), 90);
    station.marker_for = {{"Station_C", "Underworld"}, {"Station_A", "Rebel"}, {"Station_B", "Empire"}};
    inputs.placements.push_back(station);
    inputs.placements.push_back(marker(1, "Team_00_Spawn_Point_Marker", at(10, 20), 0));
    inputs.placements.push_back(marker(2, "team_00_spawn_point_marker", at(30, 40), 180));
    auto enemy_station = marker(3, "Team_01_Space_Station", at(-100, -200), 270);
    enemy_station.marker_for = station.marker_for;
    inputs.placements.push_back(enemy_station);
    inputs.placements.push_back(marker(4, "Team_01_Spawn_Point_Marker", at(-10, -20), 45));
    inputs.placements.push_back(marker(5, "Team_01_Base_Position_Marker", at(-50, -50), 45));
    inputs.placements.push_back(object(6, "Pad", "SpaceBuildable", 3, "Neutral", at(5, 5), 45));
    inputs.placements.push_back(object(7, "Rock", "SpaceProp", 3, "Neutral", at(6, 6), 0));
    inputs.placements.push_back(object(8, "Box", "SpaceStructure", 1, "Empire", at(7, 7), 0));
    auto crate = object(9, "Crate", "SpaceStructure", 3, "Neutral", at(8, 8), 30);
    crate.orientation_degrees = Vec3{whole(10), whole(20), whole(30)};
    inputs.placements.push_back(crate);
    auto beacon = object(11, "Beacon", "SpaceStructure", 4, "Wildlife", at(9, 9), 0);
    beacon.decoration = true;
    beacon.discardable = false;
    inputs.placements.push_back(beacon);
    auto debris = object(12, "Debris", "SpaceStructure", 4, "Wildlife", at(10, 10), 0);
    debris.decoration = true; // discardable by default
    inputs.placements.push_back(debris);
    inputs.placements.push_back(object(13, "Buoy", "SpaceStructure", 9, "", at(11, 11), 0));
    inputs.placements.push_back(object(14, "Hulk", "SpaceStructure", 2, "Pirates", at(12, 12), 0));
    inputs.placements.push_back(marker(10, "Team_00_Space_Station", at(300, 400), 0));
    inputs.placements.back().marker_for = station.marker_for;
    return inputs;
}

void synthetic_start() {
    const auto tables = synthetic_tables();
    const auto fixture = synthetic_fixture();
    const auto inputs = synthetic_inputs(tables);
    auto built = skirmish::build_start(fixture, inputs);
    expect(static_cast<bool>(built), "synthetic start builds");
    if (!built) {
        std::cerr << eawr::core::format_diagnostic(built.error()) << '\n';
        return;
    }
    const auto& start = built.value();

    // Players: lobby slots in slot order, then one per non-playable faction with a skirmish
    // player, in faction order.
    expect(start.players.size() == 5, "three lobby players and the Pirates and Neutral players");
    if (start.players.size() == 5) {
        const auto& rebel = start.players[0];
        expect(rebel.player == tactical::Player{1, 0, skirmish::faction_id("Rebel"), tactical::player_flag_commandable},
               "slot 1 player record");
        expect(rebel.lobby && rebel.human && rebel.start_side == "Team_00", "slot 1 is the human on Team_00");
        expect(rebel.colour && rebel.colour->constant == "MP_Color_Blue", "slot 1 takes the first MP colour");
        expect(start.players[1].colour && start.players[1].colour->constant == "MP_Color_Red",
               "slot 2 takes the second MP colour");
        expect(start.players[2].player.player_id == 3 && start.players[2].colour->constant == "MP_Color_Green",
               "slot 3 takes the third MP colour");
        expect(rebel.income && rebel.production_queue && rebel.population_cap,
               "SK-30, SK-31 (#530): a lobby player earns, builds and has a population cap");
        expect(start.players[3].player == tactical::Player{4, 2, skirmish::faction_id("Pirates"), 0U}
                   && start.players[3].owner_index == 2 && !start.players[3].lobby,
               "Pirates (index 2) is a non-commandable player after the lobby");
        expect(start.players[4].player == tactical::Player{5, 3, skirmish::faction_id("Neutral"), 0U}
                   && start.players[4].owner_index == 3,
               "Neutral (index 3) follows it; playable Empire and player-less Wildlife get none");
        expect(rebel.combat_power_tick_zero == whole(100 + 21 + 50), "station, squadron craft sum and cruiser");
        expect(rebel.combat_power_launches == whole(42), "two launched squadrons of three 7-power craft");
        expect(start.players[1].combat_power_tick_zero == whole(200 + 21 + 50), "slot 2 power");
        expect(start.players[1].combat_power_launches == Fixed{}, "Station_B has no spawner");
    }

    struct Expected {
        std::string type;
        skirmish::UnitRole role;
        tactical::PlayerId owner;
        std::uint32_t record;
    };
    // Team markers are taken in retail search order, reverse record order: slot 1 gets
    // team 0's last station (record 10) and spawn (record 2), slot 3 the ones before.
    const std::vector<Expected> expected{
        {"Station_A", skirmish::UnitRole::station, 1, 10},
        {"Fighter_Squadron", skirmish::UnitRole::free_unit, 1, 2},
        {"Cruiser", skirmish::UnitRole::fleet, 1, 2},
        {"Station_B", skirmish::UnitRole::station, 2, 3},
        {"Fighter_Squadron", skirmish::UnitRole::free_unit, 2, 4},
        {"Cruiser", skirmish::UnitRole::free_unit, 2, 4},
        {"Station_A", skirmish::UnitRole::station, 3, 0},
        {"Fighter_Squadron", skirmish::UnitRole::free_unit, 3, 1},
        {"Cruiser", skirmish::UnitRole::fleet, 3, 1},
        {"Pad", skirmish::UnitRole::map_object, 5, 6},
        {"Crate", skirmish::UnitRole::map_object, 5, 9},
        {"Beacon", skirmish::UnitRole::map_object, 5, 11}, // non-discardable decoration: Neutral
        {"Buoy", skirmish::UnitRole::map_object, 5, 13},   // owner index past the factions: Neutral
        {"Hulk", skirmish::UnitRole::map_object, 4, 14},
        // #75: each squadron company's craft, in company then member order, on the company's record.
        {"Fighter", skirmish::UnitRole::craft, 1, 2},
        {"Fighter", skirmish::UnitRole::craft, 1, 2},
        {"Fighter", skirmish::UnitRole::craft, 1, 2},
        {"Fighter", skirmish::UnitRole::craft, 2, 4},
        {"Fighter", skirmish::UnitRole::craft, 2, 4},
        {"Fighter", skirmish::UnitRole::craft, 2, 4},
        {"Fighter", skirmish::UnitRole::craft, 3, 1},
        {"Fighter", skirmish::UnitRole::craft, 3, 1},
        {"Fighter", skirmish::UnitRole::craft, 3, 1},
    };
    expect(start.units.size() == expected.size(),
           "unit count (the SpaceProp is not a unit; the Empire Box and Wildlife Debris are deleted)");
    const std::vector<std::tuple<std::uint32_t, std::string, std::string, skirmish::Removal>> removed_expected{
        {8, "Box", "Empire", skirmish::Removal::playable_faction},
        {12, "Debris", "Wildlife", skirmish::Removal::no_player}};
    std::vector<std::tuple<std::uint32_t, std::string, std::string, skirmish::Removal>> removed;
    for (const auto& entry : start.removed) removed.emplace_back(entry.record, entry.type, entry.faction, entry.reason);
    expect(removed == removed_expected, "a playable faction's object and a discardable decoration are deleted");
    for (std::size_t index = 0; index < expected.size() && index < start.units.size(); ++index) {
        const auto& unit = start.units[index];
        const auto label = "unit " + std::to_string(index + 1) + " " + expected[index].type;
        expect(unit.state.entity_id == index + 1U, label + ": entity ID");
        expect(unit.type == expected[index].type && unit.state.type_id == skirmish::type_id(expected[index].type),
               label + ": type");
        expect(unit.role == expected[index].role, label + ": role");
        expect(unit.state.owner == expected[index].owner, label + ": owner");
        expect(unit.record == expected[index].record, label + ": record");
        expect(unit.state.order == tactical::Order{}, label + ": no order");
    }
    if (start.units.size() == expected.size()) {
        // Station at its marker, rotated by the marker yaw (90 degrees: +X to +Y).
        const auto& station = start.units[6].state;
        expect(station.position == at(100, 200), "station on its marker");
        expect(station.rotation.x == Fixed{} && station.rotation.y == Fixed{}, "yaw-only quaternion");
        expect(near(station.rotation.z, 11863283) && near(station.rotation.w, 11863283), "90 degree yaw quaternion");
        const auto matrix = eawr::sim::math::to_matrix(station.rotation, station.position);
        expect(matrix && near(matrix.value().rows[0][0], 0) && near(matrix.value().rows[1][0], Fixed::scale),
               "the station's +X axis points along +Y");
        // Companies without collision bounds stand on the spawn marker (PL-02); slot 3 is team
        // 0's second player.
        expect(start.units[1].state.position == at(30, 40) && start.units[2].state.position == at(30, 40),
               "slot 1 companies on the first team-0 spawn marker in search order");
        expect(start.units[7].state.position == at(10, 20), "slot 3 on the second team-0 spawn marker in search order");
        expect(start.units[1].state.rotation.z == Fixed::from_raw(Fixed::scale) && start.units[1].state.rotation.w == Fixed{},
               "180 degree yaw is exact");
        expect(start.units[1].craft == 3 && start.units[1].craft_type == "Fighter", "squadron craft");
        expect(start.units[0].reveal_range == whole(2000) && start.units[2].reveal_range == whole(1200)
                   && start.units[1].reveal_range == whole(800),
               "reveal ranges from the tables; a squadron company reveals with its container's range");
        expect(start.units[1].combat_power == whole(21), "squadron power is the craft sum");
        expect(start.units[0].state.position == at(300, 400) && start.units[0].hull == whole(1600)
                   && start.units[0].victory_relevant,
               "slot 1 station on team 0's last station marker, with hull and victory flag");
        expect(!start.units[9].dropped_orientation_degrees, "a yaw-only map object drops nothing");
        expect(start.units[10].dropped_orientation_degrees == Vec3{whole(10), whole(20), whole(30)},
               "roll and pitch of a map object are reported");
        expect(start.units[10].yaw_degrees == whole(30), "the map object keeps its yaw");
    }

    expect(start.launches.size() == 2, "both Station_A units list their SK-23 launch");
    if (!start.launches.empty()) {
        const auto& launch = start.launches.front();
        expect(launch.spawner == 1 && launch.owner == 1 && launch.squadron == "Fighter_Squadron" && launch.count == 2
                   && launch.delay_seconds == whole(10) && launch.combat_power == whole(42),
               "launch data");
    }

    std::vector<std::pair<std::uint32_t, skirmish::MarkerUse>> markers;
    for (const auto& entry : start.markers) markers.emplace_back(entry.record, entry.use);
    const std::vector<std::pair<std::uint32_t, skirmish::MarkerUse>> expected_markers{
        {0, skirmish::MarkerUse::station}, {1, skirmish::MarkerUse::spawn}, {2, skirmish::MarkerUse::spawn},
        {3, skirmish::MarkerUse::station}, {4, skirmish::MarkerUse::spawn}, {5, skirmish::MarkerUse::base_position},
        {10, skirmish::MarkerUse::station}};
    expect(markers == expected_markers, "markers in record order with their use");

    expect(start.setup.seed == 5, "fixture seed");
    expect(start.setup.content_identity == eawr::units::content_identity(tables), "content identity is the tables'");
    expect(start.setup.players.size() == start.players.size() && start.setup.units.size() == start.units.size(),
           "setup mirrors the start");
    expect(static_cast<bool>(tactical::validate_setup(start.setup)), "setup validates");

    // The sensor table (#271): every type with a sensor range, sorted by type ID. The
    // craft authors a range but has no REVEAL; the squadron reveals through its container.
    const auto sensors = skirmish::sensor_table(tables);
    expect(sensors.size() == 3 && static_cast<bool>(tactical::validate_sensors(sensors)), "three sensor profiles");
    for (const auto* id : {"Station_A", "Cruiser", "Fighter_Squadron"}) {
        expect(std::any_of(sensors.begin(), sensors.end(),
                   [&](const tactical::SensorProfile& profile) { return profile.type_id == skirmish::type_id(id); }),
               std::string("sensor profile for ") + id);
    }
    // Sensors change the tick-zero snapshot, never the state hash.
    const auto sensed = tactical::TacticalSession::create(start.setup, sensors);
    const auto blind = tactical::TacticalSession::create(start.setup);
    expect(sensed && blind && sensed.value().state_sha256() == blind.value().state_sha256()
               && sensed.value().snapshot()->sha256() != blind.value().snapshot()->sha256(),
           "the sensor table feeds the snapshot, not the state");

    // The census with and without the start agree on everything the setup holds.
    const auto named = skirmish::census_json(start.setup, &start);
    const auto bare = skirmish::census_json(start.setup, nullptr);
    expect(named && bare, "census builds with and without the start");
    if (named && bare) {
        std::istringstream left(named.value());
        std::istringstream right(bare.value());
        std::vector<std::string> named_lines;
        std::vector<std::string> bare_lines;
        for (std::string line; std::getline(left, line);) named_lines.push_back(line);
        for (std::string line; std::getline(right, line);) bare_lines.push_back(line);
        std::size_t rows = 0;
        for (const auto& line : bare_lines) {
            if (line.rfind("  \"tick_zero\"", 0) == 0) {
                expect(std::find(named_lines.begin(), named_lines.end(), line) != named_lines.end(),
                       "tick-zero hashes agree");
            }
            if (line.rfind("    {\"player_id\"", 0) != 0 && line.rfind("    {\"entity_id\"", 0) != 0) continue;
            const auto stem = line.substr(0, line.find_last_of('}'));
            const bool found = std::any_of(named_lines.begin(), named_lines.end(),
                [&](const std::string& other) { return other.rfind(stem + ", ", 0) == 0; });
            expect(found, "named census extends the bare row: " + stem);
            ++rows;
        }
        expect(rows == start.players.size() + start.units.size(), "bare census lists every player and unit");
        expect(bare.value().find("\"source\": \"replay\"") != std::string::npos
                   && named.value().find("\"source\": \"fixture\"") != std::string::npos,
               "census source");
        expect(named.value().find("\"simulated\": false") != std::string::npos, "launches are listed as not simulated");
        expect(named.value().find("{\"record\": 8, \"type\": \"Box\", \"faction\": \"Empire\", \"reason\": \"playable_faction\"}")
                   != std::string::npos,
               "the census lists the deleted map objects");
    }
}

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
        fails(base_fixture, inputs, skirmish::diagnostic_codes::fixture, "a team without a spawn marker fails");
    }
    {
        auto inputs = base_inputs;
        inputs.placements.pop_back();
        fails(base_fixture, inputs, skirmish::diagnostic_codes::fixture, "a second team player without a station fails");
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
        auto inputs = base_inputs;
        inputs.placements[1].orientation_degrees->y = whole(1);
        fails(base_fixture, inputs, skirmish::diagnostic_codes::fixture, "a tilted marker fails");
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
        expect(bytes && bytes.value().size() == tactical::replay_header_size + 24U * setup.players.size()
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
}

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

// The committed replay of the M2 start, without the game.
void committed_fixture(const std::filesystem::path& fixtures) {
    const auto bytes = read_file(fixtures / "m2-start.eawr-replay");
    expect(bytes.has_value(), "m2-start fixture is readable");
    if (!bytes) return;
    const auto replay = tactical::parse_replay(*bytes, "m2-start.eawr-replay");
    expect(static_cast<bool>(replay), "m2-start fixture parses");
    if (!replay) return;
    const auto& setup = replay.value().setup;
    expect(replay.value().commands.empty() && replay.value().final_tick_count == 30, "setup alone, 30 ticks");
    expect(hex(setup.content_identity) == m2_content_identity, "content identity is the FoC unit-table identity");
    expect(setup.seed == skirmish::m2_fixture().seed, "fixture seed");
    expect(setup.players.size() == 7 && setup.units.size() == 62 && setup.squadrons.size() == 5,
           "7 players (#272), 35 units and the 27 craft of 5 squadrons (#75)");
    const auto session = tactical::TacticalSession::from_replay(replay.value());
    expect(session && session.value().state_sha256() == m2_tick_zero_state, "pinned tick-zero state hash");
    const auto census = skirmish::census_json(setup, nullptr);
    expect(census && census.value().find(std::string(m2_tick_zero_state)) != std::string::npos,
           "the census from the replay carries the tick-zero hash");
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

void foc_heights(const skirmish::SkirmishStart& start) {
    // SK-05: the M2 start at the FoC heights (the markers lie at z = 0; recordings S-32, S-51).
    const std::map<std::string, std::int64_t> expected{{"Calamari_Cruiser", -290}, {"Nebulon_B_Frigate", -90},
        {"Acclamator_Assault_Ship", -110}, {"Corellian_Corvette", -20}, {"Tartan_Patrol_Cruiser", 0},
        {"Skirmish_Empire_Star_Base_1", -150}, {"Skirmish_Rebel_Star_Base_1", 0}};
    std::map<std::string, int> seen;
    for (const auto& unit : start.units) {
        const auto found = expected.find(unit.type);
        if (found == expected.end()) continue;
        ++seen[unit.type];
        expect(unit.state.position.z == whole(found->second), "SK-05: " + unit.type + " starts at its Layer_Z_Adjust");
    }
    expect(seen.size() == expected.size(), "SK-05: the M2 start holds every checked type");
}

// #77 (docs/behaviour/space-victory.md): the M2 victory rules on the FoC tables, and a victory
// and a defeat by scripted damage on the start's star bases, the same at 1, 2 and 4 workers.
// SK-22 (owner, Q1 amended 2026-09-28): the Rebel MC80 stands at tick zero, the last unit of the
// Rebel fleet (entity 7), with its seven hardpoints intact and enabled: HardPoints order FL, FR
// (ion cannons), BL, BR, ML, MR (lasers), then the engines.
void foc_mc80(const skirmish::SkirmishStart& start, const eawr::units::UnitTables& tables) {
    auto content = skirmish::session_content(tables);
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return;
    const auto session = tactical::TacticalSession::create(start.setup, content.value().sensors, content.value().durability,
        content.value().motion, content.value().fog, content.value().combat);
    expect(static_cast<bool>(session), "the FoC start session builds");
    if (!session) return;
    const auto snapshot = session.value().snapshot();
    const auto instances = snapshot->instances();
    const auto mc80 = std::find_if(instances.begin(), instances.end(),
        [](const tactical::TacticalInstance& instance) { return instance.entity_id == 7; });
    expect(mc80 != instances.end() && mc80->type_id == skirmish::type_id("Calamari_Cruiser") && mc80->owner == 1,
           "SK-22: entity 7 is the Rebel Calamari_Cruiser at tick zero");
    if (mc80 == instances.end() || !mc80->durability) {
        expect(false, "SK-22: the MC80 has a durability profile");
        return;
    }
    const auto& durability = *mc80->durability;
    expect(durability.hull == whole(12750) && durability.max_hull == whole(12750), "SK-22: the MC80's 8500 x 1.5 hull (HD-01)");
    expect(durability.shields == whole(2000) && durability.max_shields == whole(2000), "SK-22: the MC80's 2000 shield");
    expect(durability.hardpoints.size() == 7 && durability.engines_online && durability.shields_online,
           "SK-22: the MC80's seven hardpoints, engines and shields online");
    for (std::size_t index = 0; index < durability.hardpoints.size(); ++index) {
        const auto& hardpoint = durability.hardpoints[index];
        expect(hardpoint.state == tactical::HardpointState::intact && hardpoint.enabled,
               "SK-22: MC80 hardpoint " + std::to_string(index) + " intact");
        expect(hardpoint.role == (index == 6 ? tactical::HardpointRole::engine : tactical::HardpointRole::weapon),
               "SK-22: MC80 hardpoint " + std::to_string(index) + " role");
    }
    expect(!durability.hardpoints.empty() && durability.hardpoints[0].health == whole(630)
               && durability.hardpoints.back().health == whole(735),
           "SK-22: MC80 weapon hardpoints 420 x 1.5, engines 490 x 1.5");
}

void foc_victory(const skirmish::SkirmishStart& start, const eawr::units::UnitTables& tables) {
    const auto rules = skirmish::victory_rules(start, tables);
    std::vector<tactical::TypeId> stations{
        skirmish::type_id("Skirmish_Rebel_Star_Base_1"), skirmish::type_id("Skirmish_Empire_Star_Base_1")};
    std::sort(stations.begin(), stations.end());
    expect(rules.condition == tactical::VictoryCondition::enemy_starbase_destroyed && rules.starbase_types == stations
               && rules.contenders == std::vector<tactical::PlayerId>{1, 2}
               && rules.humans == std::vector<tactical::PlayerId>{1},
           "VT-01 to VT-03: the two skirmish stations count; the lobby players contend; slot 1 is human");
    auto content = skirmish::session_content(tables);
    expect(static_cast<bool>(content), "FoC session content builds");
    if (!content) return;
    struct {
        eawr::sim::EntityId rebel{};
        eawr::sim::EntityId empire{};
        eawr::sim::EntityId container{};
    } ids;
    for (const auto& unit : start.units) {
        if (unit.type == "Skirmish_Rebel_Star_Base_1") ids.rebel = unit.state.entity_id;
        if (unit.type == "Skirmish_Empire_Star_Base_1") ids.empire = unit.state.entity_id;
        if (unit.type == "Orbital_Resource_Container" && ids.container == 0) ids.container = unit.state.entity_id;
    }
    expect(ids.rebel == 1 && ids.empire == 8 && ids.container != 0, "the stations are entities 1 and 8");
    const auto hit = [](const std::uint64_t tick, const std::uint64_t sequence, const eawr::sim::EntityId target) {
        return tactical::PlayerCommand{{tick, 1, sequence}, {target}, tactical::DamagePayload{whole(100000)}};
    };
    for (const auto& [target, winner] : {std::pair{ids.empire, 1U}, std::pair{ids.rebel, 2U}}) {
        // A Hutt container falls first and decides nothing (VT-03).
        const tactical::TacticalReplay replay{start.setup, 12, {hit(2, 0, ids.container), hit(5, 1, target)}};
        std::vector<std::string> finals;
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
            auto session = tactical::TacticalSession::from_replay(replay, content.value().sensors,
                content.value().durability, content.value().motion, std::nullopt, {}, rules);
            expect(static_cast<bool>(session), "FoC victory session is created");
            if (!session) return;
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            while (session.value().completed_tick() < replay.final_tick_count) {
                const auto stepped = session.value().step(executor);
                expect(static_cast<bool>(stepped), "FoC victory step succeeds");
                if (!stepped) return;
                if (session.value().completed_tick() == 5) {
                    expect(!session.value().outcome(), "the container's loss decides nothing");
                }
            }
            const auto& outcome = session.value().outcome();
            expect(outcome && outcome->winner == winner && outcome->decided_tick == 5 && outcome->deciding_unit == target,
                   "FoC: the star base " + std::to_string(target) + " falls at tick 5; player "
                       + std::to_string(winner) + " wins");
            finals.push_back(session.value().state_sha256());
        }
        expect(finals.size() == 3 && finals[0] == finals[1] && finals[1] == finals[2],
               "FoC victory: 1, 2 and 4 workers end on the same hash");
        if (!finals.empty()) std::cout << "FoC victory of player " << winner << ": final state " << finals[0] << '\n';
    }
}

/// #597 (space-movement PL-01 to PL-07) on Coruscant, both sides:
// - the search's own test holds for every searched object: its unturned placement box at its
//   point meets no earlier object's turned hull (the map objects and stations come first);
// - no two ships or stations overlap, by their turned footprints;
// - every company and craft stands within the search distance of its spawn marker.
// Because the search tests the new object's box unturned (PL-04), a craft's turned footprint may
// clip another's at the edge; those clips are printed, not failed (PL-06).
void foc_placement(const skirmish::SkirmishStart& start, const eawr::units::UnitTables& tables) {
    using Corners = std::array<std::array<double, 2>, 4>;
    const auto to_double = [](const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale); };
    // A footprint is the placement box turned by the unit's yaw: the rectangle the unit covers.
    const auto footprint = [&](const skirmish::PlacementBox& box, const skirmish::StartUnit& unit) {
        const double yaw = to_double(unit.yaw_degrees) * 3.14159265358979323846 / 180.0;
        const double c = std::cos(yaw);
        const double s = std::sin(yaw);
        const double px = to_double(unit.state.position.x);
        const double py = to_double(unit.state.position.y);
        Corners corners{};
        const std::array<std::array<Fixed, 2>, 4> local{{{box.min_x, box.min_y}, {box.max_x, box.min_y},
            {box.max_x, box.max_y}, {box.min_x, box.max_y}}};
        for (std::size_t index = 0; index < 4; ++index) {
            const double x = to_double(local[index][0]);
            const double y = to_double(local[index][1]);
            corners[index] = {px + x * c - y * s, py + x * s + y * c};
        }
        return corners;
    };
    // Separating axes: how deep two convex rectangles overlap, the least overlap along an edge
    // normal (zero or less when some normal separates them).
    const auto depth = [](const Corners& a, const Corners& b) {
        double least = 1e300;
        for (const auto* shape : {&a, &b}) {
            for (std::size_t edge = 0; edge < 4; ++edge) {
                const auto& p = (*shape)[edge];
                const auto& q = (*shape)[(edge + 1) % 4];
                const double size = std::hypot(q[0] - p[0], q[1] - p[1]);
                const double nx = (q[1] - p[1]) / size;
                const double ny = (p[0] - q[0]) / size;
                double a_low = 1e300, a_high = -1e300, b_low = 1e300, b_high = -1e300;
                for (const auto& corner : a) {
                    const double d = corner[0] * nx + corner[1] * ny;
                    a_low = std::min(a_low, d);
                    a_high = std::max(a_high, d);
                }
                for (const auto& corner : b) {
                    const double d = corner[0] * nx + corner[1] * ny;
                    b_low = std::min(b_low, d);
                    b_high = std::max(b_high, d);
                }
                least = std::min(least, std::min(a_high, b_high) - std::max(a_low, b_low));
            }
        }
        return least;
    };
    const auto box_of = [&](const skirmish::StartUnit& unit) -> std::optional<skirmish::PlacementBox> {
        if (unit.role == skirmish::UnitRole::map_object) {
            for (const auto& obstacle : tables.obstacles) {
                if (obstacle.id == unit.type) return skirmish::placement_box(obstacle);
            }
            return std::nullopt;
        }
        const auto* type = tables.find(unit.type);
        return type == nullptr ? std::nullopt : skirmish::placement_box(*type);
    };
    const auto plus = [](const Fixed left, const Fixed right) { return Fixed::from_raw(left.raw() + right.raw()); };

    // Placement order (PL-01): map objects and stations, then each company in entity order, a
    // squadron company as its craft in member order.
    std::vector<const skirmish::StartUnit*> order;
    for (const auto& unit : start.units) {
        if (unit.role == skirmish::UnitRole::map_object || unit.role == skirmish::UnitRole::station) order.push_back(&unit);
    }
    for (const auto& unit : start.units) {
        if (unit.role != skirmish::UnitRole::free_unit && unit.role != skirmish::UnitRole::fleet) continue;
        const auto squadron = std::find_if(start.setup.squadrons.begin(), start.setup.squadrons.end(),
            [&](const tactical::Squadron& entry) { return entry.container == unit.state.entity_id; });
        if (squadron == start.setup.squadrons.end()) {
            order.push_back(&unit);
            continue;
        }
        for (const auto member : squadron->members) order.push_back(&start.units[member - 1U]);
    }

    std::vector<skirmish::PlacementBox> hulls;
    std::size_t searched = 0;
    for (const auto* unit : order) {
        const auto box = box_of(*unit);
        if (!box) continue; // map objects without a footprint
        const bool search = unit->role != skirmish::UnitRole::map_object && unit->role != skirmish::UnitRole::station;
        const std::string label = std::to_string(unit->state.entity_id) + " " + unit->type;
        if (search) {
            ++searched;
            const auto& at_point = unit->state.position;
            const skirmish::PlacementBox own{plus(box->min_x, at_point.x), plus(box->min_y, at_point.y),
                plus(box->max_x, at_point.x), plus(box->max_y, at_point.y)};
            const bool clear = std::none_of(hulls.begin(), hulls.end(), [&](const skirmish::PlacementBox& hull) {
                return own.min_x < hull.max_x && hull.min_x < own.max_x && own.min_y < hull.max_y && hull.min_y < own.max_y;
            });
            expect(clear, "PL-04: " + label + "'s search box meets nothing placed before it");
            const auto marker = std::find_if(start.markers.begin(), start.markers.end(),
                [&](const skirmish::StartMarker& entry) { return entry.record == unit->record; });
            expect(marker != start.markers.end(), "PL-03: " + label + " has its marker");
            if (marker != start.markers.end()) {
                const double dx = to_double(at_point.x) - to_double(marker->position.x);
                const double dy = to_double(at_point.y) - to_double(marker->position.y);
                expect(dx * dx + dy * dy < 2500.0 * 2500.0, "PL-03: " + label + " within 2500 of its marker");
            }
        }
        auto hull = skirmish::blocker_bounds(*box, unit->state.position, unit->yaw_degrees);
        expect(static_cast<bool>(hull), "PL-05: world box of " + label);
        if (hull) hulls.push_back(hull.value());
    }
    expect(searched == 5 + 27, "PL-01: five ships and 27 craft are placed by the search");

    struct Placed {
        std::string label;
        Corners corners;
        bool craft{};
    };
    std::vector<Placed> placed;
    for (const auto& unit : start.units) {
        if (unit.role == skirmish::UnitRole::map_object) continue;
        if (const auto box = box_of(unit)) {
            placed.push_back({std::to_string(unit.state.entity_id) + " " + unit.type, footprint(*box, unit),
                unit.role == skirmish::UnitRole::craft});
        }
    }
    std::size_t ship_overlaps = 0;
    for (std::size_t left = 0; left < placed.size(); ++left) {
        for (std::size_t right = left + 1; right < placed.size(); ++right) {
            const double deep = depth(placed[left].corners, placed[right].corners);
            if (deep <= 0.0) continue;
            const bool ships = !placed[left].craft && !placed[right].craft;
            ship_overlaps += ships ? 1U : 0U;
            std::cout << (ships ? "overlap: " : "PL-06 craft edge clip: ") << placed[left].label << " and "
                      << placed[right].label << " by " << deep << '\n';
        }
    }
    expect(ship_overlaps == 0, "#597: no two starting ships or stations overlap at tick zero (both sides)");
}

// The pinned FoC start from the installation (read-only).
void foc_start(const std::filesystem::path& fixtures) {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) {
        std::cout << "FoC start: skipped (set EAWR_EAW_GAME_ROOT)\n";
        return;
    }
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return;
    auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(catalog), "FoC catalog loads");
    if (!catalog) return;
    eawr::scene::VfsAssetCache cache(filesystem.value());
    const auto access = cache.access();
    eawr::units::LoadInput input;
    input.catalog = &catalog.value().catalog;
    input.filesystem = &filesystem.value();
    input.model = access.model;
    auto tables = eawr::units::load_unit_tables(input);
    expect(static_cast<bool>(tables), "FoC unit tables load");
    if (!tables) return;
    const auto& fixture = skirmish::m2_fixture();
    auto inputs = skirmish::read_start_inputs(fixture, filesystem.value(), catalog.value().catalog, tables.value());
    expect(static_cast<bool>(inputs), "FoC start inputs read");
    if (!inputs) {
        std::cerr << eawr::core::format_diagnostic(inputs.error()) << '\n';
        return;
    }
    expect(inputs.value().placements.size() == 58, "SK-01: 58 placement records");
    // #495, PG-01: the declared extents are the minis' binary32 payloads (13000.0 and 12000.0).
    expect(inputs.value().map_extents == std::pair<std::uint32_t, std::uint32_t>{0x464B2000U, 0x464B2000U},
           "PG-01: Coruscant declares 13000 x 13000");
    {
        const std::string kuat = "data/art/maps/_mp_space_kuat.ted";
        auto record = filesystem.value().stat(kuat);
        auto bytes = filesystem.value().open(kuat);
        expect(static_cast<bool>(record) && static_cast<bool>(bytes), "the Kuat map opens");
        if (record && bytes) {
            auto map = eawr::assets::load_map(bytes.value(), eawr::assets::source_from(record.value()),
                eawr::assets::object_type_catalog(catalog.value().catalog));
            expect(static_cast<bool>(map), "the Kuat map decodes");
            if (map) {
                const std::span<const std::uint8_t> octets(
                    reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size());
                expect(skirmish::declared_extent_bits(octets, map.value())
                           == std::pair<std::uint32_t, std::uint32_t>{0x463B8000U, 0x463B8000U},
                       "PG-01: Kuat declares 12000 x 12000");
            }
        }
    }
    auto built = skirmish::build_start(fixture, inputs.value());
    expect(static_cast<bool>(built), "FoC start builds");
    if (!built) {
        std::cerr << eawr::core::format_diagnostic(built.error()) << '\n';
        return;
    }
    const auto& start = built.value();

    // Retail skirmish players: the two slots, then Pirates, Neutral, Hostile, Sarlacc and Hutts,
    // the non-playable factions with Create_Player_In_Multiplayer_Games, in faction order.
    expect(start.players.size() == 7, "two lobby players and five non-playable faction players");
    if (start.players.size() == 7) {
        const auto& rebel = start.players[0];
        const auto& empire = start.players[1];
        expect(rebel.faction == "Rebel" && rebel.player.team_id == 0 && rebel.human && rebel.start_side == "Team_00",
               "SK-10: slot 1 Rebel human on Team_00");
        expect(empire.faction == "Empire" && empire.player.team_id == 1 && !empire.human && empire.start_side == "Team_01",
               "SK-10: slot 2 Empire AI on Team_01");
        expect(rebel.colour && rebel.colour->constant == "MP_Color_Blue"
                   && rebel.colour->rgb == std::array<std::uint8_t, 3>{78, 150, 237},
               "SK-12: slot 1 MP_Color_Blue");
        expect(empire.colour && empire.colour->constant == "MP_Color_Red"
                   && empire.colour->rgb == std::array<std::uint8_t, 3>{237, 78, 78},
               "SK-12: slot 2 MP_Color_Red");
        expect(rebel.combat_power_tick_zero == whole(13525) && empire.combat_power_tick_zero == whole(9590),
               "SK-24: tick-zero AI_Combat_Power");
        expect(rebel.combat_power_launches == whole(1050) && empire.combat_power_launches == whole(1455),
               "SK-24: SK-23 launch power");
        const std::vector<std::pair<std::string, std::int32_t>> non_playable{
            {"Pirates", 2}, {"Neutral", 3}, {"Hostile", 4}, {"Sarlacc", 5}, {"Hutts", 7}};
        for (std::size_t index = 0; index < non_playable.size(); ++index) {
            const auto& player = start.players[index + 2];
            expect(player.faction == non_playable[index].first && player.owner_index == non_playable[index].second
                       && player.player.player_id == index + 3U && !player.player.commandable(),
                   "non-playable player " + non_playable[index].first);
        }
    }
    // RO-2, owner #312: the eight resource containers (TED owner 7) keep a real Hutts owner;
    // the Neutral pads, dock and gravity well go to the Neutral player. Nothing is deleted.
    expect(start.removed.empty(), "Coruscant: no map object is deleted");
    std::size_t hutt_containers = 0;
    for (const auto& unit : start.units) {
        if (unit.role != skirmish::UnitRole::map_object) continue;
        const bool container = unit.type == "Orbital_Resource_Container";
        hutt_containers += container && unit.state.owner == 7U ? 1U : 0U;
        if (!container) expect(unit.state.owner == 4U, unit.type + " goes to the Neutral player");
    }
    expect(hutt_containers == 8, "RO-2: the eight resource containers belong to the Hutts player");
    // SK-11: each team's last spawn marker in record order (52, 57), the first a marker search meets.
    const std::vector<std::pair<std::string, std::uint32_t>> lobby{
        {"Skirmish_Rebel_Star_Base_1", 48}, {"Rebel_X-Wing_Squadron", 52}, {"Rebel_X-Wing_Squadron", 52},
        {"Y-Wing_Squadron", 52}, {"Corellian_Corvette", 52}, {"Nebulon_B_Frigate", 52},
        {"Calamari_Cruiser", 52}, {"Skirmish_Empire_Star_Base_1", 54}, {"TIE_Interceptor_Squadron", 57}, {"TIE_Interceptor_Squadron", 57},
        {"Tartan_Patrol_Cruiser", 57}, {"Acclamator_Assault_Ship", 57}};
    // #75: the five squadron companies' craft follow the map objects (space-fighters FC-02).
    std::size_t craft_expected = 0;
    for (std::size_t index = 0; index < 12 && index < start.units.size(); ++index) {
        const auto* type = tables.value().find(start.units[index].type);
        if (type != nullptr && type->kind == UnitKind::squadron) craft_expected += type->members.size();
    }
    std::size_t craft = 0;
    for (const auto& unit : start.units) craft += unit.role == skirmish::UnitRole::craft ? 1U : 0U;
    expect(craft_expected > 0 && craft == craft_expected && start.setup.squadrons.size() == 5,
           "#75: every craft of the five squadron companies, one squadron each");
    expect(start.units.size() == 35 + craft_expected, "12 lobby units, 23 map objects and the squadron craft");
    for (std::size_t index = 0; index < lobby.size() && index < start.units.size(); ++index) {
        expect(start.units[index].type == lobby[index].first && start.units[index].record == lobby[index].second,
               "SK-20 to SK-22 unit " + std::to_string(index + 1) + " " + lobby[index].first);
    }
    foc_placement(start, tables.value());
    std::size_t map_objects = 0;
    for (const auto& unit : start.units) map_objects += unit.role == skirmish::UnitRole::map_object ? 1U : 0U;
    expect(map_objects == 23, "SK-04: 6 extractor pads, 7 laser pads, dock, gravity well, 8 containers");
    expect(start.launches.size() == 6, "SK-23: two station launches each and the Acclamator's two");
    expect(start.markers.size() == 10, "SK-03: station, base position and three spawn markers per team");
    // #68, #271 V-01, V-03: REVEAL stations and ships, the Y-Wing craft, and the squadrons
    // through their team containers.
    const auto sensors = skirmish::sensor_table(tables.value());
    expect(sensors.size() == 13 && static_cast<bool>(tactical::validate_sensors(sensors)),
           "13 sensor profiles: 2 stations, 5 ships, the Y-Wing craft, 5 squadrons");
    expect(start.units[0].reveal_range == whole(2000) && start.units[4].reveal_range == whole(1000)
               && start.units[1].reveal_range == whole(800),
           "V-01, V-03: station 2000, Corellian corvette 1000, X-wing squadron company 800 (Team)");
    const auto census = skirmish::census_json(start.setup, &start, sensors);
    expect(census && census.value().find("\"sensor_profiles\": 13") != std::string::npos,
           "the fixture census binds the sensor table");

    const auto bytes = tactical::write_replay({start.setup, 30, {}});
    const auto committed = read_file(fixtures / "m2-start.eawr-replay");
    expect(bytes && committed && bytes.value() == *committed, "the FoC start writes the committed replay");
    const auto session = tactical::TacticalSession::create(start.setup);
    expect(session && session.value().state_sha256() == m2_tick_zero_state,
        "the FoC start has the pinned hash (" + (session ? session.value().state_sha256() : std::string("no session")) + ")");
    foc_victory(start, tables.value());
    foc_mc80(start, tables.value());
    foc_heights(start);
    {
        auto selected = skirmish::fixture_from_options({}, filesystem.value(), catalog.value().catalog);
        expect(selected && selected.value().map == fixture.map && selected.value().map_sha256 == fixture.map_sha256
               && selected.value().seed == fixture.seed && selected.value().slots[0].fleet == fixture.slots[0].fleet,
               "omitted skirmish options retain the pinned M2 fixture");
        if (selected) {
            auto rebuilt = skirmish::build_start(selected.value(), inputs.value());
            const auto replay = rebuilt ? tactical::write_replay({rebuilt.value().setup, 30, {}})
                                        : eawr::core::Result<std::vector<std::uint8_t>>::failure(rebuilt.error());
            expect(replay && bytes && replay.value() == bytes.value(), "options default writes byte-identical M2 replay");
        }
        skirmish::FixtureOptions options;
        options.slots = fixture.slots;
        options.slots->push_back({3, "Rebel", 2, false, {}});
        const auto too_many = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
        expect(!too_many && too_many.error().message.find("more than two") != std::string::npos,
               "more than two players refused explicitly");
        options = {};
        options.map = "data/art/maps/_mp_land_naboo.ted";
        expect(!skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog), "land map refused");
        for (const std::string name : {"bespin", "kessel", "polus", "naboo", "coruscant"}) {
            options = {};
            options.map = "data/art/maps/_mp_space_" + name + ".ted";
            options.seed = 908;
            selected = skirmish::fixture_from_options(options, filesystem.value(), catalog.value().catalog);
            expect(static_cast<bool>(selected), "select FoC space map " + name);
            if (!selected) continue;
            auto chosen_inputs = skirmish::read_start_inputs(selected.value(), filesystem.value(), catalog.value().catalog, tables.value());
            expect(static_cast<bool>(chosen_inputs), "read selected FoC space map " + name);
            if (!chosen_inputs) continue;
            auto chosen_start = skirmish::build_start(selected.value(), chosen_inputs.value());
            expect(chosen_start && chosen_start.value().setup.seed == 908, "build selected FoC map " + name);
            if (!chosen_start) continue;
            std::string hash;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                eawr::platform::ThreadWorkerAdapter pool(workers);
                auto run = tactical::TacticalSession::create(chosen_start.value().setup, sensors);
                expect(static_cast<bool>(run), "selected map starts for each worker count");
                if (!run) continue;
                for (int tick = 0; tick < 8; ++tick) {
                    expect(static_cast<bool>(run.value().step(pool)), "selected map steps with each worker count");
                }
                if (hash.empty()) hash = run.value().state_sha256();
                expect(hash == run.value().state_sha256(), "selected map state independent of worker count");
            }
        }
    }
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: skirmish_start_tests <fixtures directory>\n";
        return 2;
    }
    const std::filesystem::path fixtures(argv[1]);
    synthetic_start();
    synthetic_placement();
    synthetic_failures();
    replay_round_trip();
    synthetic_heights();
    committed_fixture(fixtures);
    foc_start(fixtures);
    if (failures != 0) {
        std::cerr << failures << " skirmish start check(s) failed\n";
        return 1;
    }
    std::cout << "skirmish start contracts passed\n";
    return 0;
}
