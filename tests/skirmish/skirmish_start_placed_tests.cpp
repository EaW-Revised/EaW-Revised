#include "skirmish_start_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skirmish_start_test_support {

// MAT-08, MAT-09: a demo map's start keeps the map's own forces, owned by the editor player each
// owner index names, with no lobby, no ownership deletion and no station or free forces.
void synthetic_placed_start() {
    auto tables = synthetic_tables();
    for (auto& unit : tables.units) {
        if (unit.id != "Fighter_Squadron") continue;
        unit.members[0].offset = Vec3{whole(10), whole(0), whole(0)};
        unit.members[1].offset = Vec3{whole(0), whole(20), whole(0)};
        unit.members[2].offset = Vec3{whole(-10), whole(0), whole(0)};
    }
    // MAT-15: the loaded content's Build_Tab_Space_Units types reach the setup record.
    for (auto& unit : tables.units) {
        if (unit.id == "Cruiser" || unit.id == "Fighter_Squadron") unit.build_tab_space_units = true;
    }
    // The gate's list is generated data and may be empty: the gated ship is placed only when it names one.
    const auto disabled = skirmish::roster_disabled_units();
    const bool gated_ship = !disabled.empty();

    auto inputs = synthetic_inputs(tables);
    inputs.placements.clear();
    inputs.placements.push_back(object(0, "Cruiser", "SpaceUnit", 0, "Rebel", at(100, 100), 0));
    inputs.placements.push_back(object(1, "Cruiser", "SpaceUnit", 1, "Empire", at(-100, -100), 180));
    inputs.placements.push_back(object(2, "Fighter_Squadron", "SpaceUnit", 0, "Rebel", at(0, 500), 90));
    inputs.placements.push_back(object(3, "Box", "SpaceStructure", 3, "Neutral", at(7, 7), 0));
    inputs.placements.push_back(object(4, "Rock", "SpaceProp", 3, "Neutral", at(6, 6), 0));
    inputs.placements.push_back(object(5, "Buoy", "SpaceStructure", 9, "", at(11, 11), 0));
    inputs.placements.push_back(object(6, "Hulk", "SpaceStructure", 2, "Pirates", at(12, 12), 0));
    if (gated_ship) {
        inputs.placements.push_back(object(7, std::string(disabled.front().unit), "SpaceUnit", 1, "Empire",
                                           at(40, 40), 0));
    }
    inputs.placements.push_back(marker(8, "Team_00_Spawn_Point_Marker", at(10, 20), 0));

    skirmish::Fixture fixture;
    fixture.map = inputs.map;
    fixture.map_sha256 = inputs.map_sha256;
    fixture.seed = 5;
    fixture.victory_condition = tactical::VictoryCondition::none;
    fixture.placed = skirmish::PlacedForces{{{0U, "BasicRebel"}, {6U, "AI_Player_Underworld"}}};

    const auto types = skirmish::placed_force_types(inputs.placements);
    expect((types == std::vector<std::string>{"Box", "Buoy", "Cruiser", "Fighter_Squadron", "Hulk"}),
        "MAT-08: the placed types to load are the map objects, sorted, without props, markers or gated ships");

    auto built = skirmish::build_start(fixture, inputs);
    expect(static_cast<bool>(built), "MAT-08: a demo map's start builds from its placements alone");
    if (!built) {
        std::cerr << eawr::core::format_diagnostic(built.error()) << '\n';
        return;
    }
    const auto& start = built.value();
    expect(start.victory_condition == tactical::VictoryCondition::none, "MAT-17: the demo start has no victory condition");
    expect(start.markers.empty(), "MAT-08: no lobby markers");
    expect(start.setup.beacon_rules.empty(), "MAT-08: no beacon rule without a lobby");
    // One player per owning faction, id = editor index + 1.
    std::vector<tactical::PlayerId> ids;
    for (const auto& player : start.players) {
        ids.push_back(player.player.player_id);
        expect(!player.lobby && !player.human, "MAT-08: a demo map's players are not lobby slots");
    }
    expect((ids == std::vector<tactical::PlayerId>{1, 2, 3, 4}), "MAT-08: players for the factions that own something");
    const auto player_named = [&](const std::string_view faction) -> const skirmish::StartPlayer* {
        for (const auto& player : start.players) {
            if (player.faction == faction) return &player;
        }
        return nullptr;
    };
    const auto* rebel = player_named("Rebel");
    const auto* empire = player_named("Empire");
    const auto* neutral = player_named("Neutral");
    expect(rebel != nullptr && rebel->player.commandable() && rebel->ai_player_type == "BasicRebel",
        "MAT-09: the editor player named by index 0 runs BasicRebel");
    expect(empire != nullptr && !empire->player.commandable() && empire->ai_player_type.empty(),
        "MAT-09: a player no index names runs no AI");
    expect(neutral != nullptr && !neutral->player.commandable(), "MAT-09: neutral objects run no AI");

    std::size_t placed = 0, objects = 0, craft = 0;
    for (const auto& unit : start.units) {
        placed += unit.role == skirmish::UnitRole::placed ? 1U : 0U;
        objects += unit.role == skirmish::UnitRole::map_object ? 1U : 0U;
        craft += unit.role == skirmish::UnitRole::craft ? 1U : 0U;
        expect(skirmish::roster_disabled_reason(unit.type).empty(), "RG-04: a gated ship is not placed");
        expect(unit.type != "Rock", "SK-04: a prop is not a unit");
    }
    expect(placed == 3 && objects == 3 && craft == 3, "MAT-08: table types become forces, the rest inert map objects");
    expect(std::count_if(start.removed.begin(), start.removed.end(), [](const auto& removed) {
        return removed.reason == skirmish::Removal::roster_gated;
    }) == (gated_ship ? 1 : 0), "RG-04: a gated ship is reported as removed");

    for (const auto& unit : start.units) {
        if (unit.type == "Buoy") {
            expect(neutral != nullptr && unit.state.owner == neutral->player.player_id,
                "SK-04: an owner index past the factions is the neutral player's");
        }
        if (unit.type == "Cruiser" && unit.state.position.x > Fixed{}) {
            expect(unit.role == skirmish::UnitRole::placed && rebel != nullptr
                && unit.state.owner == rebel->player.player_id && unit.combat_power && *unit.combat_power == whole(50),
                "MAT-08: a placed cruiser is the Rebel player's force with its table power");
        }
    }
    // The squadron's craft stand on their Squadron_Offsets turned by the company's yaw (90 degrees).
    std::vector<Vec3> stands;
    for (const auto& unit : start.units) {
        if (unit.role == skirmish::UnitRole::craft) stands.push_back(unit.state.position);
    }
    constexpr std::int64_t tolerance = 4096; // the turn's sine and cosine are fixed-point approximations
    expect(stands.size() == 3 && near(stands[0].x, 0, tolerance) && near(stands[0].y, 510 * Fixed::scale, tolerance)
        && near(stands[1].x, -20 * Fixed::scale, tolerance) && near(stands[1].y, 500 * Fixed::scale, tolerance)
        && near(stands[2].x, 0, tolerance) && near(stands[2].y, 490 * Fixed::scale, tolerance),
        "MAT-08: a placed squadron's craft keep their offset slots, turned by the company's yaw");

    // The session accepts it and its hash is the same on 1, 2, 4 and 8 workers.
    std::optional<std::string> first_hash;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto session = tactical::TacticalSession::create(start.setup);
        expect(static_cast<bool>(session), "MAT-08: the demo start creates a tactical session");
        if (!session) return;
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::string last;
        for (int tick = 0; tick < 6; ++tick) {
            auto step = session.value().step(executor);
            expect(static_cast<bool>(step), "MAT-08: the demo session advances");
            if (!step) return;
            last = step.value().state_sha256;
        }
        if (!first_hash) first_hash = last;
        expect(last == *first_hash, "MAT-08: the demo session's hash is equal on 1, 2, 4 and 8 workers");
    }

    // MAT-15: the setup carries the pool-return record, sorted; a lobby start never does.
    auto expected_types = std::vector<tactical::TypeId>{skirmish::type_id("Cruiser"), skirmish::type_id("Fighter_Squadron")};
    std::sort(expected_types.begin(), expected_types.end());
    expect(start.setup.attract_pool && start.setup.attract_pool->types == expected_types,
        "MAT-15: a placed start lists the Build_Tab_Space_Units types in its setup record");
    expect(static_cast<bool>(tactical::validate_setup(start.setup)), "MAT-15: the record validates");
    auto lobby = skirmish::build_start(synthetic_fixture(), synthetic_inputs(tables));
    expect(lobby && !lobby.value().setup.attract_pool, "MAT-15: a lobby start has no pool-return record");

    // MAT-15: the demo's players hold pool accounts with no credits and no income; neutral objects hold none.
    auto account_tables = tables;
    const std::vector<std::pair<std::string, std::int64_t>> constants{{"MP_Default_Credits", 5000},
        {"Tactical_Build_Time_Multiplier", 1}, {"Space_Elevated_Vulnerability_Factor", -3},
        {"Space_Elevated_Vulnerability_Duration", 5}, {"Space_Reinforcement_Collision_Check_Distance", 200}};
    for (const auto& [tag, value] : constants) account_tables.constants.scalars.push_back({tag, whole(value), {}});
    for (auto& entry : account_tables.units) {
        if (entry.id == "Station_A") {
            eawr::units::IncomeStream stream;
            stream.base_value = whole(30);
            stream.interval_seconds = whole(10);
            entry.production.income.push_back(stream);
        }
    }
    auto account_inputs = synthetic_inputs(account_tables);
    account_inputs.placements = inputs.placements;
    account_inputs.tables = &account_tables;
    account_inputs.factions[0].space_unit_cap = 20; // Rebel and Empire; Pirates sets no cap and Neutral is neutral
    account_inputs.factions[1].space_unit_cap = 20;
    const auto account_start = skirmish::build_start(fixture, account_inputs);
    expect(static_cast<bool>(account_start), "MAT-15: the placed start builds with unit caps");
    if (account_start) {
        const auto rules = skirmish::economy_rules(account_start.value(), account_inputs, account_tables);
        expect(static_cast<bool>(rules), "MAT-15: the demo's economy builds without lobby slots or markers");
        if (rules) {
            std::vector<tactical::PlayerId> accounts;
            for (const auto& entry : rules.value().players) {
                accounts.push_back(entry.player);
                expect(entry.credits == Fixed{} && entry.ai && entry.population_cap == 20,
                    "MAT-15: a demo account has no credits and its faction's population cap");
            }
            expect((accounts == std::vector<tactical::PlayerId>{1, 2}),
                "MAT-15: the players of factions with a unit cap hold accounts, neutral objects and capless factions do not");
            expect(rules.value().income.empty(), "MAT-15: no station pays the demo's accounts");
            auto session = tactical::TacticalSession::create(account_start.value().setup, {}, {}, {}, std::nullopt, {},
                {}, {}, rules.value());
            expect(static_cast<bool>(session) && session.value().ledgers().size() == 2,
                "MAT-15: the demo session holds one ledger per account");
        }
    }

    // A placed start has no lobby: slots with it are a mistake.
    auto with_slots = fixture;
    with_slots.slots.push_back({1, "Rebel", 0, true, {}});
    expect(!skirmish::build_start(with_slots, inputs), "MAT-08: a placed start refuses lobby slots");
}

} // namespace skirmish_start_test_support
