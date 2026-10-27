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
    // FL-06: a loader-refused maximum skips that company's lobby and map references.
    {
        auto skipped_tables = synthetic_tables();
        std::erase_if(skipped_tables.units, [](const auto& type) { return type.id == "Fighter_Squadron"; });
        skipped_tables.unresolved.push_back({"Fighter_Squadron", "Max_Squad_Size", "0",
            "type skipped: expected a positive signed 32-bit whole team size"});
        auto skipped_inputs = synthetic_inputs(skipped_tables);
        auto lobby = synthetic_fixture();
        lobby.slots.front().fleet.push_back("Fighter_Squadron");
        const auto built = skirmish::build_start(lobby, skipped_inputs);
        expect(static_cast<bool>(built), "FL-06: bad squad maximum does not refuse a lobby battle");
        if (built) expect(std::none_of(built.value().units.begin(), built.value().units.end(),
            [](const auto& unit) { return unit.type == "Fighter_Squadron"; }), "FL-06: invalid companies are skipped");
        skipped_inputs.placements = {object(0, "Fighter_Squadron", "SpaceUnit", 0, "Rebel", at(0, 500), 0),
            object(1, "Cruiser", "SpaceUnit", 0, "Rebel", at(100, 500), 0)};
        lobby.slots.clear();
        lobby.placed = skirmish::PlacedForces{};
        const auto placed = skirmish::build_start(lobby, skipped_inputs);
        expect(placed && std::any_of(placed.value().units.begin(), placed.value().units.end(),
            [](const auto& unit) { return unit.type == "Cruiser"; })
            && std::none_of(placed.value().units.begin(), placed.value().units.end(),
            [](const auto& unit) { return unit.type == "Fighter_Squadron"; }),
            "FL-06: invalid placed company is skipped while its valid neighbor starts");
        expect(skipped_tables.unresolved.front().owner == "Fighter_Squadron"
            && skipped_tables.unresolved.front().value == "0", "FL-06: skip preserves named authored-value diagnostic");
    }
    // FL-06: placed companies partition successful mixed craft in stable input order.
    for (const std::size_t count : {2U, 3U, 4U, 8U, 186U}) {
        auto split_tables = synthetic_tables();
        auto alternate = split_tables.units.back();
        alternate.id = "Alternate_Fighter";
        alternate.creates_team = true;
        const auto alternate_index = static_cast<std::uint32_t>(split_tables.units.size());
        split_tables.units.push_back(alternate);
        const auto maximum = count == 186 ? 200U : 3U;
        auto* company = &*std::find_if(split_tables.units.begin(), split_tables.units.end(),
            [](const auto& type) { return type.id == "Fighter_Squadron"; });
        const auto member = company->members.front();
        company->members.assign(count, member);
        for (std::size_t i = 1; i < count; i += 2)
            company->members[i] = {alternate.id, alternate_index, std::nullopt};
        company->max_squad_size = maximum;
        // PG-08: only the first member's Create_Team enables the type-level size clamp.
        auto* craft = &*std::find_if(split_tables.units.begin(), split_tables.units.end(),
            [&](const auto& type) { return type.id == member.craft; });
        craft->creates_team = true;
        auto split_inputs = synthetic_inputs(split_tables);
        split_inputs.placements = {object(0, "Fighter_Squadron", "SpaceUnit", 0, "Rebel", at(0, 500), 0)};
        skirmish::Fixture split_fixture;
        split_fixture.map = split_inputs.map;
        split_fixture.map_sha256 = split_inputs.map_sha256;
        split_fixture.seed = 5;
        split_fixture.victory_condition = tactical::VictoryCondition::none;
        split_fixture.placed = skirmish::PlacedForces{};
        const auto split = skirmish::build_start(split_fixture, split_inputs);
        expect(static_cast<bool>(split), "FL-06: authored placed split builds");
        if (!split) continue;
        const auto& groups = split.value().setup.squadrons;
        expect(groups.size() == (count + maximum - 1) / maximum, "FL-06: placed boundary and uneven-tail group count");
        std::vector<eawr::sim::EntityId> members;
        for (const auto& group : groups) {
            expect(group.members.size() <= maximum, "FL-06: placed team maximum is honored");
            members.insert(members.end(), group.members.begin(), group.members.end());
        }
        expect(members.size() == count && std::is_sorted(members.begin(), members.end()), "FL-06: placed craft retain stable IDs and order");
        std::optional<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto session = tactical::TacticalSession::create(split.value().setup);
            expect(static_cast<bool>(session), "FL-06: placed split session creates");
            if (!session) continue;
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            for (int tick = 0; tick < 3; ++tick)
                expect(static_cast<bool>(session.value().step(executor)), "FL-06: placed teams advance");
            const auto hash = session.value().state_sha256();
            if (!reference) reference = hash;
            else expect(hash == *reference, "FL-06: placed teams agree on 1/2/4/8 workers");
        }
        const auto scaled = skirmish::ai_setup(split.value(), split_inputs, split_tables);
        craft->creates_team = false;
        const auto unscaled = skirmish::ai_setup(split.value(), split_inputs, split_tables);
        const auto power = [](const auto& setup) {
            const auto at = std::find_if(setup.content.types.begin(), setup.content.types.end(), [](const auto& type) {
                return type.type_id == skirmish::type_id("Fighter_Squadron");
            });
            return at->combat_power;
        };
        // Exact ratios cover all chosen small cases; 186/200 takes the clamp-to-one branch.
        const auto expected = power(unscaled) * eawr::script::numeric::LuaNumber(std::min<std::size_t>(maximum, count))
            / eawr::script::numeric::LuaNumber(count);
        expect(power(scaled) == expected, "PG-08: guarded tactical team-size power, including unsplit 186/200");
        expect(power(unscaled) > eawr::script::numeric::LuaNumber{}, "PG-08: a non-team-creating first craft retains whole authored power");
    }
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

    // MAT-08: capture can need the neutral endpoint without a live neutral placement.
    // Keep this synthetic: neutral is selected by its authored flag, not its name or index.
    auto capture_tables = account_tables;
    auto pad = unit_type("Capture_Pad", UnitKind::ship, "Rebel, Empire", std::nullopt);
    pad.hull = whole(20);
    pad.capture_point = true;
    pad.capture_radius = whole(100);
    pad.capture_seconds = whole(1);
    pad.influences_capture = false;
    capture_tables.units.push_back(std::move(pad));
    for (const unsigned variant : {0U, 1U, 2U, 3U, 4U}) {
        auto capture_inputs = synthetic_inputs(capture_tables);
        capture_inputs.factions[0].space_unit_cap = 20;
        capture_inputs.factions[1].space_unit_cap = 20;
        capture_inputs.placements = {
            object(0, "Capture_Pad", "SpaceBuildable", 0, "Rebel", at(0, 0), 0),
            object(1, "Cruiser", "SpaceUnit", 1, "Empire", at(20, 0), 0)};
        if (variant == 1) {
            capture_inputs.placements.push_back(object(2, "Rock", "SpaceProp", 3, "Neutral", at(5, 5), 0));
            auto excluded = marker(3, "Neutral_Marker", at(7, 7), 0);
            excluded.owner_index = 3;
            capture_inputs.placements.push_back(std::move(excluded));
        } else if (variant == 2) {
            capture_inputs.placements.push_back(object(2, "Box", "SpaceStructure", 3, "Neutral", at(500, 500), 0));
        } else if (variant == 3) {
            std::swap(capture_inputs.factions[2], capture_inputs.factions[3]);
            capture_inputs.factions[2].name = "Unclaimed";
        } else if (variant == 4) {
            capture_inputs.factions[3].neutral = false;
        }
        auto capture_fixture = fixture;
        if (variant == 3) capture_fixture.placed->ai.emplace_back(2U, "BasicRebel");
        const auto captured_start = skirmish::build_start(capture_fixture, capture_inputs);
        expect(static_cast<bool>(captured_start), "MAT-08: nonneutral capture pads admit a placed start");
        if (!captured_start) continue;
        const auto& setup = captured_start.value().setup;
        const auto rules = skirmish::economy_rules(captured_start.value(), capture_inputs, capture_tables);
        if (variant == 4) {
            expect(!rules && rules.error().message.find("capture requires a declared neutral player") != std::string::npos,
                "MAT-08: capture content with no authored neutral faction still refuses an invalid endpoint");
            continue;
        }
        expect(static_cast<bool>(rules), "MAT-08: the placed capture economy binds");
        if (!rules) continue;
        const tactical::PlayerId neutral_id = variant == 3 ? 3U : 4U;
        expect(setup.players.size() == 3 && setup.players[0].player_id == 1
            && setup.players[1].player_id == 2 && rules.value().pads.neutral == neutral_id,
            "MAT-08: one data-selected neutral player, preserving both combatant IDs");
        expect(setup.units.size() == (variant == 2 ? 3U : 2U),
            "MAT-08: neutral props and markers stay excluded while real neutral objects stay live");
        expect(setup.players.back().player_id == neutral_id && !setup.players.back().commandable()
            && rules.value().players.size() == 2,
            "MAT-08/15: the neutral endpoint has neither AI control nor a pool account");
        if (variant == 0) {
            auto without_ai_data = capture_tables;
            without_ai_data.ai_credit_multiplier.reset();
            auto human_accounts = captured_start.value();
            for (auto& player : human_accounts.players) {
                if (player.player.player_id != neutral_id) player.human = true;
            }
            expect(static_cast<bool>(skirmish::economy_rules(human_accounts, capture_inputs, without_ai_data)),
                "MAT-15: a neutral endpoint never requires AI difficulty data for human accounts");
        }
        std::optional<std::string> capture_hash;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto session = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, rules.value());
            expect(static_cast<bool>(session), "MAT-08: capture session loads without neutral placements");
            if (!session) continue;
            eawr::platform::ThreadWorkerAdapter executor(workers);
            bool neutralized = false;
            for (unsigned tick = 0; tick < 80; ++tick) {
                expect(static_cast<bool>(session.value().step(executor)), "MAT-08: placed pad capture advances");
                neutralized = neutralized || session.value().units().front().owner == neutral_id;
            }
            expect(neutralized && session.value().units().front().owner == 2,
                "MAT-08: an enemy-owned pad neutralizes through the declared endpoint and is captured");
            const auto hash = session.value().state_sha256();
            if (!capture_hash) capture_hash = hash;
            expect(hash == *capture_hash, "MAT-08: absent-neutral-placement capture agrees on 1/2/4/8 workers");
        }
    }

    // A placed start has no lobby: slots with it are a mistake.
    auto with_slots = fixture;
    with_slots.slots.push_back({1, "Rebel", 0, true, {}});
    expect(!skirmish::build_start(with_slots, inputs), "MAT-08: a placed start refuses lobby slots");
}

} // namespace skirmish_start_test_support
