#include "skirmish_start_support.hpp"

#include "eawr/data/tag_trace.hpp"

#include <chrono>

namespace skirmish_start_test_support {

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
    inputs.factions = {{"Rebel", true, false, false, std::nullopt, "BasicRebel"}, {"Empire", true, false, false, std::nullopt, "BasicEmpire"},
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

void synthetic_roster_gate() {
    eawr::units::UnitTables ability_tables;
    auto authored = unit_type("Admonitor_Star_Destroyer", UnitKind::ship, "Empire", 100);
    eawr::units::Ability unsupported;
    unsupported.type = "POWER_TO_WEAPONS";
    unsupported.modifiers.push_back({"UNMODELLED_MULTIPLIER", whole(2)});
    eawr::units::Ability supported;
    supported.type = "TURBO";
    authored.abilities = {unsupported, supported};
    ability_tables.units.push_back(authored);
    const auto ordinary = eawr::units::ability_table(ability_tables);
    expect(ordinary && ordinary.value().profiles.size() == 1 && ordinary.value().profiles[0].abilities.size() == 1
        && ordinary.value().profiles[0].abilities[0].kind == tactical::AbilityKind::turbo,
        "AB-26: ordinary loader omits an unmodelled ability while retaining supported abilities");
    ability_tables.units[0].abilities[0].modifiers.clear();
    const auto modelled = eawr::units::ability_table(ability_tables);
    expect(modelled && modelled.value().profiles.size() == 1 && modelled.value().profiles[0].abilities.size() == 2,
        "RG-03: control models both abilities before applying the roster policy");
    const auto allowed = eawr::units::ability_table(ability_tables, {}, [](std::string_view unit, std::string_view ability) {
        return skirmish::roster_ability_reason(unit, ability).empty();
    });
    expect(allowed && allowed.value().profiles.size() == 1 && allowed.value().profiles[0].abilities.size() == 2
        && skirmish::roster_ability_reason("Admonitor_Star_Destroyer", "POWER_TO_WEAPONS").empty(),
        "RG-03/WHE-22: roster admission retains the supported hero mode and another supported ability");
    auto tables = synthetic_tables();
    auto fixture = synthetic_fixture();
    for (const auto& row : skirmish::roster_disabled_units()) {
        tables.units.push_back(unit_type(std::string(row.unit), UnitKind::ship, "Rebel", 10));
        fixture.slots[0].fleet.push_back(std::string(row.unit));
        expect(!skirmish::roster_disabled_reason(row.unit).empty(), "each generated disabled ship has a reason");
    }
    auto inputs = synthetic_inputs(tables);
    tables.units.push_back(unit_type("Kedalbe_Battleship", UnitKind::ship, "Rebel", 10));
    tables.units.push_back(unit_type("Vengeance_Frigate", UnitKind::ship, "Rebel", 10));
    inputs.faction_forces[0].space_skirmish_default_forces.push_back("Kedalbe_Battleship");
    inputs.faction_forces[0].space_skirmish_default_forces.push_back("Krayt_Class_Destroyer");
    fixture.slots[0].fleet.push_back("Vengeance_Frigate");
    fixture.free_starting_units = true;
    auto built = skirmish::build_start(fixture, inputs);
    expect(static_cast<bool>(built), "gated starting fleet builds by skipping its disabled ships");
    if (!built) return;
    for (const auto& unit : built.value().units) {
        expect(skirmish::roster_disabled_reason(unit.type).empty(), "starting forces contain no gated ship");
    }
    for (const std::string_view carrier : {"Kedalbe_Battleship", "Vengeance_Frigate"}) {
        expect(std::any_of(built.value().units.begin(), built.value().units.end(),
            [&](const auto& unit) { return unit.type == carrier; }),
            "MD-01, RG-05: supported mass-driver default and authored fleets are deployed");
    }
    const auto ai = skirmish::ai_setup(built.value(), inputs, tables);
    for (const auto& type : ai.content.types) {
        expect(skirmish::roster_disabled_reason(type.name).empty(), "AI discovery never offers a gated ship");
    }
    expect(!skirmish::roster_disabled_reason("Krayt_Class_Destroyer").empty()
        && skirmish::roster_disabled_reason("kRaYt_cLaSs_dEsTrOyEr") == skirmish::roster_disabled_reason("Krayt_Class_Destroyer"),
        "ship gate uses case-insensitive XML identity");
    expect(!skirmish::roster_ability_reason("Kedalbe_Battleship", "LEECH_SHIELDS").empty()
        && !skirmish::roster_ability_reason("Vengeance_Frigate", "STEALTH").empty(),
        "RG-03: supported mass-driver ships retain independent optional-ability gates");
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

    // WSS-35: use content for all factions, including modded names; humans and
    // non-lobby players do not acquire controllers from a faction's Basic_AI.
    auto ai_start = start;
    auto ai_inputs = inputs;
    ai_start.players[2].faction = "uNdErWoRlD";
    ai_inputs.factions.push_back({"Underworld", true, false, false, std::nullopt, "AI_Player_Underworld"});
    auto ai = skirmish::ai_setup(ai_start, ai_inputs, tables);
    expect(ai.players[0].player_type.empty() && !ai.players[0].ai, "WSS-35: the human Rebel has no AI controller");
    expect(ai.players[1].player_type == "BasicEmpire", "WSS-35: Empire retains its authored controller");
    expect(ai.players[2].player_type == "AI_Player_Underworld", "WSS-35: Underworld gets its authored controller");
    expect(ai.players[3].player_type.empty() && !ai.players[3].ai, "WSS-35: non-lobby players retain their control policy");
    expect(ai.players[0].human && !ai.players[1].human && !ai.players[2].human
        && !ai.players[3].human && !ai.players[4].human,
        "GS-05: only the human lobby player contributes to the proposal cap's human count");
    ai_start.players[3].human = true;
    ai = skirmish::ai_setup(ai_start, ai_inputs, tables);
    expect(!ai.players[3].human, "GS-05: non-playable faction players are not active human lobby players");

    auto ffa_tables = tables;
    ffa_tables.units.push_back(unit_type("Station_C", UnitKind::station, "Underworld", 200));
    auto ffa_inputs = ai_inputs;
    ffa_inputs.tables = &ffa_tables;
    ffa_inputs.faction_forces.push_back({"Underworld", {}});
    auto third_station = marker(30, "Team_02_Space_Station", at(1000, 0), 0);
    third_station.marker_for = {{"Station_C", "Underworld"}};
    ffa_inputs.placements.push_back(third_station);
    ffa_inputs.placements.push_back(marker(31, "Team_02_Spawn_Point_Marker", at(1000, 100), 0));
    auto ffa_fixture = fixture;
    ffa_fixture.slots[2].faction = "Underworld";
    ffa_fixture.slots[2].team = 2;
    ffa_fixture.slots[2].fleet.clear();
    const auto ffa_start = skirmish::build_start(ffa_fixture, ffa_inputs);
    expect(static_cast<bool>(ffa_start), "GS-05 / WSS-62: Rebel/Empire/Underworld three-team FFA builds");
    if (!ffa_start) std::cerr << eawr::core::format_diagnostic(ffa_start.error()) << '\n';
    if (ffa_start) {
        const auto ffa_ai = skirmish::ai_setup(ffa_start.value(), ffa_inputs, ffa_tables);
        expect(ffa_ai.players.size() == 5 && ffa_ai.players[0].human && !ffa_ai.players[0].ai
            && ffa_ai.players[1].ai && !ffa_ai.players[1].human && ffa_ai.players[1].player_type == "BasicEmpire"
            && ffa_ai.players[2].ai && !ffa_ai.players[2].human && ffa_ai.players[2].player_type == "AI_Player_Underworld"
            && !ffa_ai.players[3].human && !ffa_ai.players[4].human,
            "GS-05 / WSS-35: FFA preserves both AI controllers and excludes the non-playable players from humans");
        expect(ffa_start.value().players[0].player.team_id == 0 && ffa_start.value().players[1].player.team_id == 1
            && ffa_start.value().players[2].player.team_id == 2,
            "WSS-62: the three factions retain distinct FFA teams");
    }
    ai_start.players[0].human = false;
    ai = skirmish::ai_setup(ai_start, ai_inputs, tables);
    expect(ai.players[0].player_type == "BasicRebel", "WSS-35: AI Rebel retains its authored controller");
    ai_inputs.factions.back().name = "Custom";
    ai_inputs.factions.back().basic_ai = "CustomController";
    ai_start.players[2].faction = "CUSTOM";
    ai = skirmish::ai_setup(ai_start, ai_inputs, tables);
    expect(ai.players[2].player_type == "CustomController", "WSS-35: faction lookup does not hardcode the retail pair");
    ai_inputs.factions.back().basic_ai.clear();
    ai = skirmish::ai_setup(ai_start, ai_inputs, tables);
    expect(ai.players[2].player_type.empty(), "WSS-35: an absent Basic_AI does not invent a fallback controller");

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
        expect(rebel.combat_power_tick_zero == whole(200 + 21 + 50), "team stations, squadron craft sum and cruiser");
        expect(rebel.combat_power_launches == whole(84), "both team stations launch two squadrons of three 7-power craft");
        expect(start.players[1].combat_power_tick_zero == whole(200 + 21 + 50), "slot 2 power");
        expect(start.players[1].combat_power_launches == Fixed{}, "Station_B has no spawner");
    }

    struct Expected {
        std::string type;
        skirmish::UnitRole role;
        tactical::PlayerId owner;
        std::uint32_t record;
    };
    // WSS-61: both team station markers belong to the first teammate. SK-11
    // retains reverse record order; the teammates receive distinct spawn markers.
    const std::vector<Expected> expected{
        {"Station_A", skirmish::UnitRole::station, 1, 10},
        {"Station_A", skirmish::UnitRole::station, 1, 0},
        {"Fighter_Squadron", skirmish::UnitRole::free_unit, 1, 2},
        {"Cruiser", skirmish::UnitRole::fleet, 1, 2},
        {"Station_B", skirmish::UnitRole::station, 2, 3},
        {"Fighter_Squadron", skirmish::UnitRole::free_unit, 2, 4},
        {"Cruiser", skirmish::UnitRole::free_unit, 2, 4},
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
        const auto& station = start.units[1].state;
        expect(station.position == at(100, 200), "station on its marker");
        expect(station.rotation.x == Fixed{} && station.rotation.y == Fixed{}, "yaw-only quaternion");
        expect(near(station.rotation.z, 11863283) && near(station.rotation.w, 11863283), "90 degree yaw quaternion");
        const auto matrix = eawr::sim::math::to_matrix(station.rotation, station.position);
        expect(matrix && near(matrix.value().rows[0][0], 0) && near(matrix.value().rows[1][0], Fixed::scale),
               "the station's +X axis points along +Y");
        // Companies without collision bounds stand on the spawn marker (PL-02); slot 3 is team
        // 0's second player.
        expect(start.units[2].state.position == at(30, 40) && start.units[3].state.position == at(30, 40),
               "slot 1 companies on the first team-0 spawn marker in search order");
        expect(start.units[7].state.position == at(10, 20), "slot 3 on the second team-0 spawn marker in search order");
        expect(start.units[2].state.rotation.z == Fixed::from_raw(Fixed::scale) && start.units[2].state.rotation.w == Fixed{},
               "180 degree yaw is exact");
        expect(start.units[2].craft == 3 && start.units[2].craft_type == "Fighter", "squadron craft");
        expect(start.units[0].reveal_range == whole(2000) && start.units[3].reveal_range == whole(1200)
                   && start.units[2].reveal_range == whole(800),
               "reveal ranges from the tables; a squadron company reveals with its container's range");
        expect(start.units[2].combat_power == whole(21), "squadron power is the craft sum");
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

void setup_contract() {
    // WSS-54/61: a 2v2 roster shares two team stations and has four distinct spawns.
    {
        const auto tables = synthetic_tables();
        auto inputs = synthetic_inputs(tables);
        inputs.placements.pop_back(); // one station for each of the two teams
        inputs.placements.push_back(marker(15, "Team_01_Spawn_Point_Marker", at(-30, -40), 225));
        inputs.lobby_colours.push_back({"MP_Color_Orange", {10, 11, 12}});
        auto fixture = synthetic_fixture();
        fixture.slots.push_back({4, "Empire", 1, false, {}});
        const auto start = skirmish::build_start(fixture, inputs);
        expect(static_cast<bool>(start), "2v2 starts with one station and two spawns per team");
        if (start) {
            std::vector<tactical::PlayerId> station_owners, spawn_owners;
            for (const auto& unit : start.value().units)
                if (unit.role == skirmish::UnitRole::station) station_owners.push_back(unit.state.owner);
            for (const auto& entry : start.value().markers)
                if (entry.use == skirmish::MarkerUse::spawn) spawn_owners.push_back(entry.player);
            std::sort(spawn_owners.begin(), spawn_owners.end());
            expect(station_owners == std::vector<tactical::PlayerId>{1, 2}, "2v2 team stations belong to first teammates");
            expect(spawn_owners == std::vector<tactical::PlayerId>{1, 2, 3, 4}, "all four players receive distinct spawns");
            expect(start.value().players[3].colour->constant == "MP_Color_Orange", "fourth player's authored colour survives start");
            std::string expected;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                eawr::platform::ThreadWorkerAdapter pool(workers);
                auto session = tactical::TacticalSession::create(start.value().setup, skirmish::sensor_table(tables));
                expect(static_cast<bool>(session), "2v2 session accepts all players");
                if (!session) continue;
                for (int tick = 0; tick < 8; ++tick) expect(static_cast<bool>(session.value().step(pool)), "2v2 ticks succeed");
                if (expected.empty()) expected = session.value().state_sha256();
                expect(session.value().state_sha256() == expected, "2v2 is deterministic on 1/2/4/8 workers");
                const auto bytes = tactical::write_replay(session.value().record());
                const auto replay = bytes ? tactical::parse_replay(bytes.value())
                    : eawr::core::Result<tactical::TacticalReplay>::failure(bytes.error());
                expect(replay && replay.value().setup == start.value().setup, "2v2 replay preserves team and player setup");
            }
        }
        auto overflow = fixture; overflow.slots.back().team = 0; overflow.slots.back().faction = "Rebel";
        const auto rejected = skirmish::build_start(overflow, inputs);
        expect(!rejected && rejected.error().message.find("spawn") != std::string::npos,
            "a third teammate cannot reuse one of two spawns");
        // Custom maps may author four starts; stock maps author at most three.
        fixture.slots[1].faction = "Rebel";
        for (std::uint32_t team = 2; team < 4; ++team) {
            auto station = marker(20 + team * 2, "Team_0" + std::to_string(team) + "_Space_Station", at(1000 * team, 0), 0);
            station.marker_for = {{"Station_A", "Rebel"}};
            inputs.placements.push_back(station);
            inputs.placements.push_back(marker(21 + team * 2, "Team_0" + std::to_string(team) + "_Spawn_Point_Marker", at(1000 * team, 100), 0));
            fixture.slots[team].team = team;
            fixture.slots[team].faction = "Rebel";
        }
        for (const std::size_t count : {3U, 4U}) {
            auto free_for_all = fixture; free_for_all.slots.resize(count);
            const auto built = skirmish::build_start(free_for_all, inputs);
            expect(built && std::count_if(built.value().units.begin(), built.value().units.end(),
                [](const auto& unit) { return unit.role == skirmish::UnitRole::station; }) == static_cast<int>(count),
                "three/four authored FFA teams each receive their station");
            if (!built) continue;
            std::vector<eawr::sim::EntityId> fleet;
            for (const auto& slot : free_for_all.slots) {
                const auto ship = std::find_if(built.value().units.begin(), built.value().units.end(),
                    [&](const auto& unit) { return unit.state.owner == slot.slot
                        && (unit.role == skirmish::UnitRole::fleet || unit.role == skirmish::UnitRole::free_unit); });
                expect(ship != built.value().units.end(), "every FFA player receives a fleet");
                if (ship != built.value().units.end()) fleet.push_back(ship->state.entity_id);
            }
            if (fleet.size() != count) continue;
            // WSS-62: same-faction FFA players remain hostile; fog belongs to each team.
            auto separated = built.value().setup;
            for (auto& unit : separated.units) unit.position = at(10000 * unit.owner, 0);
            const auto distant = tactical::TacticalSession::create(separated, skirmish::sensor_table(tables));
            expect(static_cast<bool>(distant), "separated FFA sensors bind");
            if (distant) {
                for (const auto& observer : free_for_all.slots) {
                    const auto visible = distant.value().snapshot()->visible_entities(observer.slot);
                    for (const auto& target : free_for_all.slots) {
                        expect(tactical::players_hostile(distant.value().snapshot()->players(), observer.slot, target.slot)
                            == (observer.slot != target.slot), "FFA hostility is pairwise and independent of faction");
                        const auto entity = fleet[target.slot - 1U];
                        expect((std::find(visible.begin(), visible.end(), entity) != visible.end())
                            == (observer.slot == target.slot), "FFA teams do not share distant visibility");
                    }
                }
            }
            auto contact = separated;
            for (auto& unit : contact.units) unit.position = at(0, 0);
            const auto revealed = tactical::TacticalSession::create(contact, skirmish::sensor_table(tables));
            expect(static_cast<bool>(revealed), "FFA contact sensors bind");
            if (revealed) for (const auto& observer : free_for_all.slots) {
                const auto visible = revealed.value().snapshot()->visible_entities(observer.slot);
                for (const auto entity : fleet)
                    expect(std::find(visible.begin(), visible.end(), entity) != visible.end(),
                        "contact reveals every FFA fleet to every observer, including fourth team");
            }
            std::string expected;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                eawr::platform::ThreadWorkerAdapter pool(workers);
                auto session = tactical::TacticalSession::create(built.value().setup);
                expect(static_cast<bool>(session), "FFA session starts");
                if (!session) continue;
                for (int tick = 0; tick < 8; ++tick) expect(static_cast<bool>(session.value().step(pool)), "FFA session advances");
                if (expected.empty()) expected = session.value().state_sha256();
                expect(expected == session.value().state_sha256(), "FFA deterministic on 1/2/4/8 workers");
            }
        }
    }
    {
        const auto directory = std::filesystem::temp_directory_path()
            / ("eawr-setup-maps-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory / "art/maps");
        const auto write = [&](const char* name, std::uint32_t kind, std::uint32_t capacity, bool custom) {
            std::vector<unsigned char> root;
            const auto integer = [&](std::uint32_t value) {
                for (unsigned i = 0; i < 4; ++i) root.push_back(static_cast<unsigned char>((value >> (8U * i)) & 255U));
            };
            const auto field = [&](unsigned char id, std::uint32_t value) { root.push_back(id); root.push_back(4); integer(value); };
            field(0, 0x0201); field(1, kind); field(2, capacity); field(3, 5);
            root.push_back(11); root.push_back(1); root.push_back(custom ? 1 : 0);
            std::ofstream file(directory / "art/maps" / name, std::ios::binary);
            const std::array<unsigned char, 4> zero{};
            file.write(reinterpret_cast<const char*>(zero.data()), 4);
            const auto size = static_cast<std::uint32_t>(root.size());
            for (unsigned i = 0; i < 4; ++i) file.put(static_cast<char>((size >> (8U * i)) & 255U));
            file.write(reinterpret_cast<const char*>(root.data()), static_cast<std::streamsize>(root.size()));
        };
        write("unrelated_name.ted", 2, 4, false);
        write("_mp_space_misleading_land.ted", 1, 4, false);
        write("_mp_space_custom.ted", 2, 4, true);
        write("_mp_space_single.ted", 2, 1, false);
        std::filesystem::create_directories(directory / "xml");
        std::ofstream(directory / "xml/factionfiles.xml")
            << "<Faction_Files><File>factions.xml</File></Faction_Files>";
        std::ofstream(directory / "xml/factions.xml")
            << "<Factions><Faction Name=\"Rebel\"><Is_Playable>Yes</Is_Playable></Faction>"
               "<Faction Name=\"Empire\"><Is_Playable>Yes</Is_Playable></Faction></Factions>";
        const std::string defaults_xml =
            "<MP_Default_Allow_Heroes>no</MP_Default_Allow_Heroes>"
            "<MP_Default_Allow_SuperWeapons>yes</MP_Default_Allow_SuperWeapons>"
            "<MP_Default_Free_Starting_Units>no</MP_Default_Free_Starting_Units>"
            "<MP_Default_Pre_Built_Base>yes</MP_Default_Pre_Built_Base>"
            "<MP_Default_Allow_Random_Events>no</MP_Default_Allow_Random_Events>"
            "<MP_Default_Credits>7000</MP_Default_Credits>"
            "<MP_Default_Start_Tech_Level>2</MP_Default_Start_Tech_Level>"
            "<MP_Default_Max_Tech_Level>7</MP_Default_Max_Tech_Level>"
            "<MP_Default_Game_Timer>23</MP_Default_Game_Timer>"
            "<MP_Default_Win_Condition_Int_Param>-4</MP_Default_Win_Condition_Int_Param>"
            "<MP_Default_Win_Condition_Float_Param>2.5</MP_Default_Win_Condition_Float_Param>"
            "<MP_Default_Allow_Auto_Resolve>2</MP_Default_Allow_Auto_Resolve>"
            "<MP_Default_Win_Condition>0</MP_Default_Win_Condition>"
            "<MP_Default_Space_Tactical_Win_Condition>SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED</MP_Default_Space_Tactical_Win_Condition>";
        const auto constants = [&](const std::string& xml) {
            std::ofstream file(directory / "xml/gameconstants.xml");
            file << "<GameConstants>" << xml;
            for (const auto* suffix : {"Eight", "Gray", "Yellow", "Purple", "Cyan", "Orange", "Green", "Red", "Blue"})
                file << "<MP_Color_" << suffix << ">1,2,3</MP_Color_" << suffix << ">";
            file << "<MP_Color_Custom>4,5,6</MP_Color_Custom></GameConstants>";
        };
        constants(defaults_xml);
        std::ofstream(directory / "xml/gameobjectfiles.xml") << "<Game_Object_Files><File>policy.xml</File></Game_Object_Files>";
        std::ofstream(directory / "xml/policy.xml") << "<Objects>"
            "<Container Name=\"Policy_Hero\"><Is_Named_Hero>yes</Is_Named_Hero></Container>"
            "<Container Name=\"Policy_Derived\"><Variant_Of_Existing_Type>Policy_Hero</Variant_Of_Existing_Type></Container>"
            "<HeroUnit Name=\"Policy_Not_Hero\"><Is_Named_Hero>no</Is_Named_Hero></HeroUnit>"
            "<UpgradeObject Name=\"Policy_Super\"><Is_Skirmish_Tactical_Super_Weapon>yes</Is_Skirmish_Tactical_Super_Weapon></UpgradeObject>"
            "</Objects>";
        const eawr::vfs::MountSpec mount{.layer_id = "synthetic-mod", .data_root = directory,
            .loose_logical_prefix = "data", .active_archives = {}};
        auto filesystem = eawr::vfs::Vfs::mount(std::span<const eawr::vfs::MountSpec>(&mount, 1));
        expect(static_cast<bool>(filesystem), "synthetic map corpus mounts");
        if (filesystem) {
            const eawr::data::Catalog catalog;
            auto official = skirmish::setup_maps(filesystem.value(), catalog);
            expect(official && official.value().size() == 1 && official.value()[0].path.ends_with("unrelated_name.ted")
                && official.value()[0].name == "unrelated_name", "WSS-03/06: header selects arbitrary stem and preserves absent-name fallback");
            skirmish::SetupMapQuery custom_query; custom_query.custom = true;
            auto custom = skirmish::setup_maps(filesystem.value(), catalog, custom_query);
            expect(custom && custom.value().size() == 1 && custom.value()[0].path.ends_with("_mp_space_custom.ted"),
                "WSS-07: official and custom maps in the same layer are classified only by authored flags");
            const auto defaults = skirmish::read_match_defaults(filesystem.value());
            expect(defaults && !defaults.value().allow_heroes && !defaults.value().free_starting_units
                && defaults.value().allow_superweapons && defaults.value().pre_built_base
                && defaults.value().credits == whole(7000) && defaults.value().game_timer == 23
                && defaults.value().start_tech == 2 && defaults.value().max_tech == 7 && defaults.value().win_integer == -4
                && defaults.value().win_float == Fixed::from_raw(Fixed::scale * 5 / 2) && defaults.value().auto_resolve == 2,
                "WSS-22: complete copied defaults come from authored data, including hidden fields");
            const auto palette = skirmish::read_lobby_colours(filesystem.value());
            expect(palette && palette.value().size() == 9 && palette.value()[0].constant == "MP_Color_Blue"
                && palette.value()[4].constant == "MP_Color_Cyan" && palette.value()[8].constant == "MP_Color_Eight",
                "WSS-20: selector palette is stable across XML order and ignores extra mod colours");
            auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
            expect(static_cast<bool>(loaded), "synthetic inherited policy catalog loads");
            if (loaded) {
                skirmish::FixtureOptions land;
                land.map = "data/art/maps/_mp_space_misleading_land.ted";
                auto land_fixture = skirmish::fixture_from_options(land, filesystem.value(), loaded.value().catalog);
                expect(!land_fixture && land_fixture.error().message == "skirmish map must be a space map",
                    "explicit CLI map path rejects an authored land map with a clear diagnostic");
                skirmish::Fixture fixture; fixture.map = "data/art/maps/unrelated_name.ted";
                eawr::units::UnitTables empty_tables;
                std::vector<eawr::data::tag_trace::Entry> policy_trace;
                {
                    eawr::data::tag_trace::Recording recording;
                    for (int run = 0; run < 2; ++run) {
                        auto policy = skirmish::read_start_inputs(fixture, filesystem.value(), loaded.value().catalog, empty_tables);
                        expect(policy && std::binary_search(policy.value().named_heroes.begin(), policy.value().named_heroes.end(), skirmish::type_id("Policy_Derived"))
                            && !std::binary_search(policy.value().named_heroes.begin(), policy.value().named_heroes.end(), skirmish::type_id("Policy_Not_Hero"))
                            && policy.value().superweapons == std::vector<tactical::TypeId>{skirmish::type_id("Policy_Super")},
                            "WSS-29/30: traced purchase policy preserves inherited flags and explicit false, including cached resolves");
                    }
                    policy_trace = recording.finish();
                }
                expect(std::none_of(policy_trace.begin(), policy_trace.end(), [](const auto& entry) {
                    return entry.kind == eawr::data::tag_trace::Kind::object && entry.logical_path == "data/xml/policy.xml";
                }), "purchase policy: catalog-only candidates do not become full scene trace roots");
                for (const auto flag : {"Is_Named_Hero", "Is_Skirmish_Tactical_Super_Weapon"}) {
                    expect(std::any_of(policy_trace.begin(), policy_trace.end(), [&](const auto& entry) {
                        return entry.kind == eawr::data::tag_trace::Kind::used && entry.element == flag;
                    }), "purchase policy: the actual hero and superweapon flag reads remain traced");
                }
            }
            constants("<MP_Default_Allow_Heroes>maybe</MP_Default_Allow_Heroes>");
            auto invalid = eawr::vfs::Vfs::mount(std::span<const eawr::vfs::MountSpec>(&mount, 1));
            expect(invalid && !skirmish::read_match_defaults(invalid.value()), "malformed or incomplete match defaults reject deterministically");
        }
        std::filesystem::remove_all(directory);
    }
    eawr::assets::Map authored;
    authored.kind = eawr::assets::MapKind::space;
    authored.lobby.capacity = 9;
    authored.lobby.levels = 5;
    authored.lobby.custom = false;
    authored.lobby.new_markers = false;
    authored.lobby.game_types = "";
    expect(skirmish::setup_map_eligible(authored), "WSS-03: ordinary list does not restrict new markers or game types");
    skirmish::SetupMapQuery query;
    query.custom = true;
    expect(!skirmish::setup_map_eligible(authored, query), "official map excluded from custom list");
    authored.lobby.custom = true;
    expect(skirmish::setup_map_eligible(authored, query) && !skirmish::setup_map_eligible(authored),
        "WSS-07: header custom flag determines list independently of source layer and filename");
    query.minimum_capacity = 10;
    expect(!skirmish::setup_map_eligible(authored, query), "minimum capacity is an authored header filter");
    query.minimum_capacity = 2; query.minimum_levels = 6;
    expect(!skirmish::setup_map_eligible(authored, query), "minimum levels is an authored header filter");
    query.minimum_levels = 0; query.owner = 2;
    expect(!skirmish::setup_map_eligible(authored, query), "missing required owner rejects restricted query");
    authored.lobby.owner = 2;
    expect(skirmish::setup_map_eligible(authored, query), "matching authored owner admits restricted query");
    query.owner.reset(); query.terrain = 4;
    expect(!skirmish::setup_map_eligible(authored, query), "missing required terrain rejects restricted query");
    authored.lobby.terrain = 4;
    expect(skirmish::setup_map_eligible(authored, query), "matching authored terrain admits restricted query");
    query.terrain.reset(); query.new_markers_only = true;
    expect(!skirmish::setup_map_eligible(authored, query), "new-marker restriction applies only when requested");
    query.new_markers_only = false; query.game_type = "MODE";
    expect(!skirmish::setup_map_eligible(authored, query), "nonempty game-type filter excludes empty authored text");
    authored.lobby.game_types = "OTHER MODE";
    expect(skirmish::setup_map_eligible(authored, query), "game-type filter matches authored substring");
    query.game_type.clear(); authored.kind = eawr::assets::MapKind::land;
    expect(!skirmish::setup_map_eligible(authored, query), "space query excludes land header regardless of name");
    authored.kind = eawr::assets::MapKind::space; authored.lobby.capacity.reset();
    expect(!skirmish::setup_map_eligible(authored, query), "unknown or malformed capacity never becomes a lobby default");
    skirmish::SetupMap map;
    map.path = "data/art/maps/_mp_space_example.ted";
    map.name = "Example";
    map.metadata.capacity = 9;
    map.metadata.new_markers = true;
    map.metadata.start_positions = std::vector<eawr::assets::Vec2f>(3);
    map.teams = {{0, {"Rebel", "Empire"}}, {1, {"Rebel", "Empire"}}, {2, {"Rebel"}}};
    const std::array maps{map};
    skirmish::SetupSelection selection;
    selection.map = map.path;
    auto options = skirmish::setup_options(selection, maps);
    expect(options && options.value().map == map.path && options.value().slots->size() == 2
        && options.value().slots->at(0).human && !options.value().slots->at(1).human
        && options.value().slots->at(0).fleet.empty(), "setup passes map and both slots without M2 demonstration fleets");
    selection.slots[0].faction = "Empire";
    selection.slots[1].faction = "Rebel";
    selection.slots[0].team = 1;
    selection.slots[1].team = 2;
    options = skirmish::setup_options(selection, maps);
    expect(options && options.value().slots->at(0).team == 1 && options.value().slots->at(0).faction == "Empire"
        && options.value().slots->at(1).team == 2, "non-default faction and authored team survive mapping");
    auto bad = selection;
    bad.map = "data/art/maps/_mp_land_example.ted";
    expect(!skirmish::setup_options(bad, maps), "unlisted/land maps rejected");
    bad = selection; bad.slots[1].team = 1;
    expect(!skirmish::setup_options(bad, maps), "allied-only lobby rejected");
    bad = selection; bad.slots[1].team = 3;
    expect(!skirmish::setup_options(bad, maps), "unavailable team rejected");
    bad = selection; bad.slots[1].faction = "Empire";
    expect(!skirmish::setup_options(bad, maps), "faction without station on selected team rejected");
    bad = selection; bad.slots[0].faction = "Underworld";
    expect(!skirmish::setup_options(bad, maps), "unavailable Underworld station rejected");
    bad = selection; bad.slots[1].human = true;
    expect(!skirmish::setup_options(bad, maps), "second human rejected");
    bad = selection; bad.slots[0].human = false;
    expect(!skirmish::setup_options(bad, maps), "missing local human rejected");
    bad = selection; bad.slots[1].slot = 1;
    expect(!skirmish::setup_options(bad, maps), "duplicate slot rejected");
    bad = selection; bad.slots[0].fleet = {"Example_Ship"};
    expect(!skirmish::setup_options(bad, maps), "hidden fleet override rejected");
    auto unavailable = maps;
    unavailable[0].unavailable = "Map data is malformed.";
    expect(!skirmish::setup_options(selection, unavailable), "unavailable map rejected");
    auto roster = selection;
    roster.slots.push_back({4, "Rebel", 2, false, {}});
    expect(static_cast<bool>(skirmish::setup_options(roster, maps)), "WSS-17/18: same-faction teammates with sparse row indices start");
    roster.slots.back().faction = "Empire";
    expect(!skirmish::setup_options(roster, maps), "WSS-18: mixed-faction teammates block Start");
    roster.slots.back().team = 0;
    expect(static_cast<bool>(skirmish::setup_options(roster, maps)), "three homogeneous occupied starts are permitted");
    roster.slots.back().human = true;
    expect(!skirmish::setup_options(roster, maps), "WSS-14: nonhost local humans are rejected");
    roster.slots.back().human = false;
    roster.slots.back().team = 0xFFFFFFFFU;
    expect(!skirmish::setup_options(roster, maps), "WSS-17: unassigned teams cannot admit a new-marker Start");
    roster.slots.resize(1);
    expect(!skirmish::setup_options(roster, maps), "WSS-16: host with all other rows Open cannot Start");
    roster = selection;
    for (std::uint32_t row = 3; row <= 8; ++row) roster.slots.push_back({row, "Rebel", 2, false, {}});
    expect(static_cast<bool>(skirmish::setup_options(roster, maps)), "WSS-08: capacity nine permits eight local occupied rows");
    auto limited = maps;
    limited[0].teams[2].spawn_capacity = 2;
    const auto overflow = skirmish::setup_options(roster, limited);
    expect(!overflow && overflow.error().message.find("spawn markers") != std::string::npos,
        "setup rejects team spawn overflow despite sufficient total map capacity");
    auto custom = map;
    custom.metadata.capacity = 4;
    custom.metadata.start_positions->resize(4);
    custom.teams = {{0, {"Rebel"}, 1}, {1, {"Empire"}, 1}, {2, {"Rebel"}, 1}, {3, {"Empire"}, 1}};
    skirmish::SetupSelection free_for_all;
    free_for_all.map = custom.path;
    free_for_all.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, false, {}},
        {3, "Rebel", 2, false, {}}, {4, "Empire", 3, false, {}}};
    const std::array custom_maps{custom};
    expect(static_cast<bool>(skirmish::setup_options(free_for_all, custom_maps)), "four authored custom starts admit four-player FFA");
    free_for_all.slots.push_back({5, "Rebel", 0, false, {}});
    expect(!skirmish::setup_options(free_for_all, custom_maps), "custom map rejects a roster beyond authored capacity");
    roster.slots.push_back({9, "Rebel", 2, false, {}});
    expect(!skirmish::setup_options(roster, maps), "WSS-08: ninth local row stays unavailable");
    auto smaller = map; smaller.metadata.capacity = 2;
    smaller.metadata.start_positions->resize(2);
    roster = {{}, {{1, "Rebel", 0, true, {}}, {8, "Empire", 7, false, {}}, {9, "Rebel", 2, false, {}}}};
    skirmish::repair_setup_selection(roster, smaller);
    expect(roster.map == smaller.path && roster.slots.size() == 2 && roster.slots[0].slot == 1
        && roster.slots[1].slot == 2 && roster.slots[1].team == 1,
        "WSS-08/09: map change drops excess rows, relocates survivor and clamps excessive team to last authored start");
    auto legacy = maps; legacy[0].metadata.new_markers = false;
    expect(!skirmish::setup_options(selection, legacy), "WSS-19: legacy branch reports unsupported instead of guessing new-marker validation");
    auto colours = selection;
    expect(static_cast<bool>(skirmish::select_setup_colour(colours, 1, 4)), "WSS-20: host can select a faction-independent palette entry");
    expect(static_cast<bool>(skirmish::select_setup_colour(colours, 2, 4)) && colours.slots[1].colour_index == 0U
        && colours.slots[0].colour_index == 4U, "WSS-21: edited duplicate moves to first unused entry, preserving another row's choice");
    colours.slots[1].colour_index = 4;
    expect(!skirmish::setup_options(colours, maps), "unrepaired local colour collision cannot Start");
    expect(!skirmish::select_setup_colour(colours, 2, 9), "palette indices outside nine authored entries reject deterministically");
    expect(!skirmish::select_setup_colour(colours, 8, 0), "Open rows have no colour record to edit");
    colours.slots[0].colour_index = 0;
    expect(!skirmish::select_setup_colour(colours, 2, 0, 1), "exhausted palette produces an input error");
    const auto tables = synthetic_tables();
    auto fixture = synthetic_fixture();
    const auto inputs = synthetic_inputs(tables);
    const auto original = skirmish::build_start(fixture, inputs);
    fixture.slots[0].colour_index = 2;
    fixture.slots[1].colour_index = 0;
    const auto changed = skirmish::build_start(fixture, inputs);
    expect(original && changed && original.value().setup == changed.value().setup
        && changed.value().players[0].colour->rgb == std::array<std::uint8_t, 3>{7, 8, 9}
        && changed.value().players[1].colour->constant == "MP_Color_Blue",
        "WSS-47/48: selected palette reaches players while tactical replay/hash state stays byte-identical");
    fixture.slots[0].colour_index = 3;
    expect(!skirmish::build_start(fixture, inputs), "runtime rejects a selected colour absent from the loaded palette");
    skirmish::MatchOptions defaults;
    defaults.credits = whole(7000); defaults.game_timer = 23; defaults.pre_built_base = true;
    auto edit = skirmish::begin_setup_options(selection, defaults);
    edit.value.allow_heroes = false; edit.value.free_starting_units = false; edit.custom = true;
    expect(!selection.match, "WSS-24: editing a copy and discarding it leaves staging unchanged");
    skirmish::accept_setup_options(selection, edit);
    options = skirmish::setup_options(selection, maps);
    expect(options && options.value().match && !options.value().match->allow_heroes
        && !options.value().match->free_starting_units && options.value().match->game_timer == 23
        && selection.custom_options, "WSS-22/24: Accept carries supported policy and retains hidden fields");
    edit = skirmish::begin_setup_options(selection, defaults);
    skirmish::reset_setup_options(edit, defaults);
    expect(edit.value == defaults && !edit.custom && !selection.match->allow_heroes,
        "WSS-22/24: Defaults resets only the copy until Accept");
    skirmish::accept_setup_options(selection, edit);
    expect(selection.match == defaults && !selection.custom_options, "accepted Defaults clears custom state");
    auto no_forces_fixture = synthetic_fixture();
    no_forces_fixture.match = defaults;
    no_forces_fixture.match->free_starting_units = false;
    const auto no_forces = skirmish::build_start(no_forces_fixture, inputs);
    expect(no_forces && std::none_of(no_forces.value().units.begin(), no_forces.value().units.end(),
        [](const auto& unit) { return unit.role == skirmish::UnitRole::free_unit; }),
        "WSS-28: selected free-force off omits human and AI faction default rosters");
    expect(no_forces && std::count_if(no_forces.value().units.begin(), no_forces.value().units.end(),
        [](const auto& unit) { return unit.role == skirmish::UnitRole::station; }) == 3
        && std::count_if(no_forces.value().units.begin(), no_forces.value().units.end(),
        [](const auto& unit) { return unit.role == skirmish::UnitRole::fleet; }) == 2,
        "WSS-27/28: free-force off preserves explicit fleets and the existing station policy");

    auto no_station_inputs = inputs;
    std::erase_if(no_station_inputs.placements, [](const auto& placement) {
        return placement.type.ends_with("Space_Station");
    });
    no_forces_fixture.match->pre_built_base = false;
    no_forces_fixture.victory_condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    const auto no_stations = skirmish::build_start(no_forces_fixture, no_station_inputs);
    expect(no_stations && no_stations.value().victory_condition == tactical::VictoryCondition::all_enemy_units_destroyed
        && std::none_of(no_stations.value().units.begin(), no_stations.value().units.end(),
            [](const auto& unit) { return unit.role == skirmish::UnitRole::station; }),
        "WSS-27/WBF-08: copied base-off policy needs no station markers and retains explicit typed victory");
    no_forces_fixture.pre_built_base = false;
    no_forces_fixture.match->pre_built_base = true;
    expect(!skirmish::build_start(no_forces_fixture, no_station_inputs),
        "WSS-27: copied base-on policy validates station markers before accessing them");

    auto policy_tables = synthetic_tables();
    for (const auto* tag : {"MP_Default_Credits", "Tactical_Build_Time_Multiplier", "Space_Elevated_Vulnerability_Factor",
        "Space_Elevated_Vulnerability_Duration", "Space_Reinforcement_Collision_Check_Distance"})
        policy_tables.constants.scalars.push_back({tag, whole(std::string_view(tag) == "Space_Elevated_Vulnerability_Factor" ? -1 : 1), {}});
    auto special = unit_type("Policy_Upgrade", UnitKind::ship, "Rebel, Empire", 0);
    special.production.upgrade_object = true;
    special.production.increments_tech = true;
    policy_tables.units.push_back(special);
    for (auto& type : policy_tables.units) {
        type.hull = whole(100);
        type.production.build_cost_multiplayer = whole(100);
        type.production.build_time_seconds = whole(1);
        if (type.kind == UnitKind::station) {
            type.production.buildable = {{"Rebel", {"Cruiser", "Policy_Upgrade", "Fighter_Squadron"}},
                {"Empire", {"Cruiser", "Policy_Upgrade", "Fighter_Squadron"}}};
        }
    }
    auto policy_inputs = synthetic_inputs(policy_tables);
    for (auto& faction : policy_inputs.factions) faction.space_unit_cap = 25;
    policy_inputs.named_heroes = {skirmish::type_id("Cruiser")};
    policy_inputs.superweapons = {skirmish::type_id("Policy_Upgrade")};
    auto policy_fixture = synthetic_fixture(); policy_fixture.slots.resize(2);
    for (const bool allowed : {true, false}) {
        policy_fixture.match = defaults;
        policy_fixture.match->allow_heroes = allowed;
        policy_fixture.match->allow_superweapons = allowed;
        auto policy_start = skirmish::build_start(policy_fixture, policy_inputs);
        expect(static_cast<bool>(policy_start), "policy fixture starts");
        if (!policy_start) continue;
        auto rules = skirmish::economy_rules(policy_start.value(), policy_inputs, policy_tables);
        expect(static_cast<bool>(rules), "shared policy catalog binds");
        if (!rules) { std::cerr << rules.error().message << '\n'; continue; }
        auto session = tactical::TacticalSession::create(policy_start.value().setup, {}, {}, {}, std::nullopt, {}, {}, {}, rules.value());
        expect(static_cast<bool>(session), "policy admission session starts");
        if (!session) continue;
        for (const auto& unit : policy_start.value().units) {
            if (unit.role != skirmish::UnitRole::station) continue;
            const auto* menu = rules.value().menu(unit.state.type_id, policy_start.value().setup.players[unit.state.owner - 1].faction_id);
            expect(menu && (menu->find(skirmish::type_id("Cruiser")) != nullptr) == allowed
                && (menu->find(skirmish::type_id("Policy_Upgrade")) != nullptr) == allowed
                && menu->find(skirmish::type_id("Fighter_Squadron")), "WSS-29/30: policy filters listing while ordinary entries remain");
            expect(session.value().build_allowed(unit.state.owner, unit.state.entity_id, skirmish::type_id("Cruiser")) == allowed
                && session.value().build_allowed(unit.state.owner, unit.state.entity_id, skirmish::type_id("Policy_Upgrade")) == allowed,
                "WSS-29/30: authoritative eligibility applies to human and AI producers");
        }
        const auto replay_fixture = skirmish::replay_fixture(policy_fixture, policy_inputs, policy_start.value().setup);
        expect(replay_fixture && replay_fixture.value().slots.size() == policy_fixture.slots.size(),
            "recorded lobby players resolve the matching faction bindings");
        if (!replay_fixture) continue;
        const auto rebuilt = skirmish::build_start(replay_fixture.value(), policy_inputs);
        const auto rebound = rebuilt ? skirmish::economy_rules(rebuilt.value(), policy_inputs, policy_tables)
            : eawr::core::Result<tactical::EconomyRules>::failure(rebuilt.error());
        expect(rebound && rebound.value() == rules.value(), "recorded policy rebuilds the same purchase menus");
        if (!rebound) continue;
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 4; ++tick) {
            const auto step = session.value().step(eawr::sim::InlineExecutor{});
            expect(static_cast<bool>(step), "copied-policy source tick runs");
            if (step) hashes.push_back(step.value().state_sha256);
        }
        const auto bytes = tactical::write_replay(session.value().record());
        const auto parsed = bytes ? tactical::parse_replay(bytes.value())
            : eawr::core::Result<tactical::TacticalReplay>::failure(bytes.error());
        expect(static_cast<bool>(parsed), "policy session records and parses");
        if (!parsed) continue;
        auto replayed = tactical::TacticalSession::from_replay(parsed.value(), {}, {}, {}, std::nullopt,
            {}, {}, {}, rebound.value());
        expect(static_cast<bool>(replayed), "non-default policy replay accepts matching purchase content");
        if (replayed) {
            std::vector<std::string> reproduced;
            for (int tick = 0; tick < 4; ++tick) {
                const auto step = replayed.value().step(eawr::sim::InlineExecutor{});
                if (step) reproduced.push_back(step.value().state_sha256);
            }
            const auto recorded = tactical::write_replay(replayed.value().record());
            expect(reproduced == hashes && recorded && recorded.value() == bytes.value(),
                "policy replay reproduces every tick hash and replay byte");
        }
        auto wrong_rules = rebound.value();
        wrong_rules.match_policy.allow_heroes = !wrong_rules.match_policy.allow_heroes;
        const auto wrong = tactical::TacticalSession::from_replay(parsed.value(), {}, {}, {}, std::nullopt,
            {}, {}, {}, wrong_rules);
        expect(!wrong && wrong.error().message.find("match policy") != std::string::npos,
            "a replay with a different bound policy fails clearly before ticking");
    }
}


} // namespace skirmish_start_test_support
