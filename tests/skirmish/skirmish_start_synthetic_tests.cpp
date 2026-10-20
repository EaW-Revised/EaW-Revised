#include "skirmish_start_support.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/core/result.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/sim/match_identity.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/battle/arrival_plan.hpp"
#include "eawr/sim/tactical/visibility.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/units/unit_tables_common.hpp"
#include "eawr/units/unit_tables_combat.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

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

void synthetic_special_sources() {
    auto tables = synthetic_tables();
    auto source = unit_type("Test_Planetary_A", UnitKind::ship, "Rebel", std::nullopt);
    source.special_weapon = true;
    source.planetary_weapon = eawr::units::UnitType::PlanetaryWeapon{0, 1, 4};
    eawr::units::Weapon weapon;
    weapon.min_recharge_seconds = whole(30); weapon.max_recharge_seconds = whole(30);
    weapon.pulse_count = 3; weapon.pulse_delay_seconds = Fixed::from_decimal("0.52").value();
    source.weapon = weapon;
    tables.units.push_back(source);
    auto inputs = synthetic_inputs(tables);
    inputs.factions[0].standalone_special_weapons[0] = source.id;
    auto fixture = synthetic_fixture();
    auto fallback = skirmish::build_start(fixture, inputs);
    expect(fallback && std::count_if(fallback.value().units.begin(), fallback.value().units.end(),
        [](const auto& unit) { return unit.role == skirmish::UnitRole::special_weapon; }) == 2,
        "WSW-01: one source per playable Rebel player, no implicit Empire or neutral source");
    if (!fallback) return;
    for (const auto& unit : fallback.value().units) if (unit.role == skirmish::UnitRole::special_weapon)
        expect(unit.state.position == Vec3{} && unit.state.rotation == eawr::sim::math::identity_quat()
            && !unit.hull && !unit.combat_power && !unit.reveal_range,
            "WSW-01: absent map source falls back to the origin without ship health, power or sensor");
    auto overflow_fixture = fixture;
    overflow_fixture.slots.push_back({4, "Rebel", 0, false, {}});
    inputs.lobby_colours.push_back({"Test_Overflow_Colour", {10, 11, 12}});
    const auto overflow_source = skirmish::build_start(overflow_fixture, inputs);
    expect(overflow_source && std::count_if(overflow_source.value().units.begin(), overflow_source.value().units.end(),
        [](const auto& unit) { return unit.state.owner == 4 && unit.role == skirmish::UnitRole::special_weapon; }) == 1
        && std::none_of(overflow_source.value().units.begin(), overflow_source.value().units.end(),
            [](const auto& unit) { return unit.state.owner == 4 && unit.role != skirmish::UnitRole::special_weapon; }),
        "WSW-01/WSS-54: an overflow playable player retains its standalone source but gets no fleet");
    auto backdrop = object(40, "Test_Planet", "SpaceProp", 3, "Neutral", at(700, -900), 90);
    backdrop.special_weapon_source = true; backdrop.layer_z_adjust = whole(-2500);
    backdrop.orientation_degrees->x = whole(12);
    backdrop.orientation_degrees->y = whole(-7);
    inputs.placements.push_back(backdrop);
    auto placed = skirmish::build_start(fixture, inputs);
    expect(static_cast<bool>(placed), "WSW-01: source-bearing planet backdrop creates source objects");
    if (!placed) return;
    for (const auto& unit : placed.value().units) if (unit.role == skirmish::UnitRole::special_weapon)
        expect(unit.state.position == Vec3{whole(700), whole(-900), whole(-2500)}
            && unit.yaw_degrees == whole(90) && unit.state.rotation.x != Fixed{} && unit.state.rotation.y != Fixed{},
            "WSW-01: a tilted scenic backdrop supplies height-adjusted position and full orientation");
    fixture.match = skirmish::MatchOptions{}; fixture.match->allow_superweapons = false;
    auto denied = skirmish::build_start(fixture, inputs);
    expect(denied && denied.value().units.size() == placed.value().units.size(),
        "WSW-01: match purchase denial does not erase persistent source identity");
    inputs.factions[0].standalone_special_weapons[1] = source.id;
    expect(!skirmish::build_start(fixture, inputs), "WSW-44: unsupported multi-instance mapping stays gated");
    const auto combat = eawr::units::combat_table(tables);
    expect(static_cast<bool>(combat), "WSW-14: planetary content binds");
    if (!combat) return;
    const auto* profile = combat.value().find(skirmish::type_id(source.id));
    expect(profile && profile->planetary_weapon && profile->planetary_weapon->recharge_frames == 900
        && profile->planetary_weapon->pulse_delay_frames == 16,
        "WSW-19: planetary timing uses nearest frames, independently of ordinary gun clocks");
    tactical::TacticalSetup setup;
    setup.players = placed.value().setup.players;
    for (const auto& unit : placed.value().units) if (unit.role == skirmish::UnitRole::special_weapon) setup.units.push_back(unit.state);
    std::vector<std::string> hashes;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto session = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, combat.value());
        expect(static_cast<bool>(session), "WSW-14: dormant planetary session creates");
        if (!session) return;
        for (const auto& instance : session.value().snapshot()->instances())
            expect(instance.planetary_weapon && instance.planetary_weapon->readiness == whole(1)
                && instance.planetary_weapon->state.pulses_left == 0 && !instance.durability,
                "WSW-14: planetary source starts fully ready without an autonomous burst");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        for (int frame = 0; frame != 35; ++frame) {
            auto step = session.value().step(executor);
            expect(static_cast<bool>(step), "WSW-14: dormant source steps");
            if (!step) return;
            if (workers == 1) hashes.push_back(step.value().state_sha256);
            else expect(hashes[frame] == step.value().state_sha256, "WSW-14: source hashes equal across worker counts");
            expect(session.value().snapshot()->projectiles().empty(), "WSW-14: dormant source never fires autonomously");
        }
        const auto bytes = tactical::write_replay(session.value().record());
        expect(static_cast<bool>(bytes), "WSW-14: source replay writes");
        if (!bytes) return;
        const auto replay = tactical::parse_replay(bytes.value(), "synthetic special source");
        expect(static_cast<bool>(replay), "WSW-14: source replay round trips");
        if (!replay) return;
        auto reproduced = tactical::TacticalSession::from_replay(replay.value(), {}, {}, {}, std::nullopt, combat.value());
        expect(static_cast<bool>(reproduced), "WSW-14: replay rebuilds source state from content");
        if (!reproduced) return;
        for (const auto& hash : hashes) {
            const auto step = reproduced.value().step(executor);
            expect(step && step.value().state_sha256 == hash, "WSW-14: every dormant source replay tick matches");
        }
    }
    auto changed = tables;
    changed.units.back().planetary_weapon->valid_targets ^= 8;
    expect(eawr::units::content_identity(changed) != eawr::units::content_identity(tables),
        "WSW-14: planetary admission metadata binds replay content identity");
}

void synthetic_roster_gate() {
    eawr::units::UnitTables ability_tables;
    auto authored = unit_type("Admonitor_Star_Destroyer", UnitKind::ship, "Empire", 100);
    eawr::units::Ability unsupported;
    unsupported.type = "POWER_TO_WEAPONS";
    unsupported.modifiers.push_back({"UNMODELLED_MULTIPLIER", whole(2)});
    unsupported.modifiers.push_back({"SPEED_MULTIPLIER", whole(2)});
    eawr::units::Ability supported;
    supported.type = "TURBO";
    authored.abilities = {unsupported, supported};
    ability_tables.units.push_back(authored);
    const auto ordinary = eawr::units::ability_table(ability_tables);
    expect(ordinary && ordinary.value().profiles.size() == 1 && ordinary.value().profiles[0].abilities.size() == 2
        && ordinary.value().profiles[0].abilities[0].kind == tactical::AbilityKind::power_to_weapons
        && ordinary.value().profiles[0].abilities[0].modifiers.speed == whole(2)
        && ordinary.value().profiles[0].abilities[1].kind == tactical::AbilityKind::turbo,
        "AB-26: unknown modifiers retain the ability and known modifiers that follow them");
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
    auto hunter = unit_type("TIE_Interceptor", UnitKind::ship, "Empire", 100);
    eawr::units::Ability hunt;
    hunt.type = "HUNT";
    hunter.abilities.push_back(hunt);
    ability_tables.units.push_back(hunter);
    const auto live_content = eawr::units::ability_table(ability_tables, {},
        [](std::string_view unit, std::string_view ability) { return skirmish::roster_ability_reason(unit, ability).empty(); });
    expect(static_cast<bool>(live_content), "WAB-50: Hunt binds through the skirmish content ability filter");
    if (live_content) {
        const auto* profile = live_content.value().find(eawr::assets::object_type_crc("TIE_Interceptor"));
        expect(profile && eawr::sim::tactical::ability_slot(*profile, eawr::sim::tactical::AbilityKind::hunt)
                   && skirmish::roster_ability_reason("TIE_Interceptor", "HUNT").empty(),
            "RG-03/WAB-50: the supported Hunt slot survives the roster gate");
    }
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
    tables.units.push_back(unit_type("Krayt_Class_Destroyer", UnitKind::ship, "Rebel", 10));
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
    for (const std::string_view carrier : {"Kedalbe_Battleship", "Vengeance_Frigate", "Krayt_Class_Destroyer"}) {
        expect(std::any_of(built.value().units.begin(), built.value().units.end(),
            [&](const auto& unit) { return unit.type == carrier; }),
            "MD-01, WAD-33, RG-05: supported mass-driver and SPECIAL default/authored fleets are deployed");
    }
    const auto ai = skirmish::ai_setup(built.value(), inputs, tables);
    for (const auto& type : ai.content.types) {
        expect(skirmish::roster_disabled_reason(type.name).empty(), "AI discovery never offers a gated ship");
    }
    expect(skirmish::roster_disabled_reason("Krayt_Class_Destroyer").empty()
        && skirmish::roster_disabled_reason("kRaYt_cLaSs_dEsTrOyEr") == skirmish::roster_disabled_reason("Krayt_Class_Destroyer"),
        "WAD-33: reviewed SPECIAL admission uses case-insensitive XML identity");
    expect(skirmish::roster_ability_reason("Kedalbe_Battleship", "LEECH_SHIELDS").empty()
        && skirmish::roster_ability_reason("Vengeance_Frigate", "STEALTH").empty(),
        "RG-03, WLS-01, WST-01: both implemented optional abilities are available");
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

    // MPS-6: domains can be extended by GC; order and worker scheduling are irrelevant.
    const auto identity = skirmish::match_identity(inputs);
    expect(identity == skirmish::match_identity(inputs), "match identity is stable across two runs");
    expect(!eawr::core::build_identity().empty(), "configure-time build identity is available to the handshake");
    auto reordered = identity;
    std::reverse(reordered.domains.begin(), reordered.domains.end());
    expect(eawr::sim::compute_match_identity(reordered) == identity, "domain input order has one canonical identity");
    expect(!eawr::sim::validate_match_identity(reordered), "noncanonical domain order is refused");
    auto duplicate = identity;
    duplicate.domains.push_back(duplicate.domains.front());
    duplicate = eawr::sim::compute_match_identity(std::move(duplicate));
    expect(!eawr::sim::validate_match_identity(duplicate), "duplicate domain names are refused");
    auto bad_version = identity;
    bad_version.version = 2;
    bad_version = eawr::sim::compute_match_identity(std::move(bad_version));
    expect(!eawr::sim::validate_match_identity(bad_version), "unsupported match identity versions are refused");
    for (std::size_t index = 0; index < identity.domains.size(); ++index) {
        auto changed = identity;
        changed.domains[index].digest[0] ^= 1U;
        changed = eawr::sim::compute_match_identity(std::move(changed));
        const auto mismatch = eawr::sim::describe_mismatch(identity, changed);
        expect(changed.sha256 != identity.sha256 && mismatch
            && mismatch->code == eawr::sim::match_identity_mismatch_code
            && mismatch->message.find(identity.domains[index].name) != std::string::npos,
            "each changed domain flips the identity and names the mismatch");
        changed = identity;
        ++changed.domains[index].version;
        expect(eawr::sim::compute_match_identity(changed).sha256 != identity.sha256,
            "each domain version participates in the combined identity");
    }
    auto campaign = identity;
    campaign.domains.push_back(eawr::sim::identity_domain("eawr-campaign-setup", "campaign", 1));
    campaign = eawr::sim::compute_match_identity(std::move(campaign));
    const auto additional = eawr::sim::describe_mismatch(identity, campaign);
    expect(additional && additional->message.find("eawr-campaign-setup") != std::string::npos,
        "additional GC domains are named");
    expect(eawr::sim::describe_mismatch(eawr::sim::MatchAgreement{"build-a", identity},
        eawr::sim::MatchAgreement{"build-b", identity}).has_value(), "handshake rejects a different build");

    const auto recorded_setup = skirmish::recording_setup(fixture, start);
    const auto encoded = tactical::write_replay({recorded_setup, 8, {}});
    auto different_arrivals = start;
    different_arrivals.arrivals.rows.clear();
    different_arrivals.arrivals.add(1, "Unplanned_Visual_Type", 120, eawr::battle::ArrivalSource::reinforcement_pool);
    const auto replanned_setup = skirmish::recording_setup(fixture, different_arrivals);
    const auto replanned_bytes = tactical::write_replay({replanned_setup, 8, {}});
    expect(replanned_setup == recorded_setup && encoded && replanned_bytes && encoded.value() == replanned_bytes.value(),
        "arrival plans never enter tactical setup or replay bytes");
    auto original_session = tactical::TacticalSession::create(recorded_setup);
    auto replanned_session = tactical::TacticalSession::create(replanned_setup);
    expect(original_session && replanned_session, "arrival metadata comparison starts both sessions");
    if (original_session && replanned_session) for (int tick = 0; tick < 8; ++tick) {
        const auto original = original_session.value().step(eawr::sim::InlineExecutor{});
        const auto replanned = replanned_session.value().step(eawr::sim::InlineExecutor{});
        expect(original && replanned && original.value().state_sha256 == replanned.value().state_sha256,
            "arrival metadata does not change canonical simulation hashes");
    }
    if (original_session && replanned_session) {
        const auto original_checkpoint = original_session.value().save_checkpoint_state();
        const auto replanned_checkpoint = replanned_session.value().save_checkpoint_state();
        expect(original_checkpoint && replanned_checkpoint
            && original_checkpoint.value() == replanned_checkpoint.value(),
            "arrival plans never enter tactical checkpoint bytes");
    }

    const auto parsed_identity = encoded ? tactical::parse_replay(encoded.value())
        : eawr::core::Result<tactical::TacticalReplay>::failure(encoded.error());
    expect(parsed_identity && parsed_identity.value().setup.match_identity == identity,
        "MTID round trips in v4/v5 replay metadata");
    if (parsed_identity) {
        auto changed_inputs = inputs;
        changed_inputs.ai_data_digest = "changed AI XML";
        const auto refused = skirmish::replay_fixture(fixture, changed_inputs, parsed_identity.value().setup);
        expect(!refused && refused.error().code == eawr::sim::match_identity_mismatch_code
            && refused.error().message.find("ai-data") != std::string::npos,
            "playback refuses mismatched installed content before activation");
    }
    if (encoded) {
        auto corrupt = encoded.value();
        const auto header_size = static_cast<std::size_t>(corrupt[10]) + (static_cast<std::size_t>(corrupt[11]) << 8U);
        corrupt[header_size - 1] ^= 1U;
        const auto refused = tactical::parse_replay(corrupt);
        expect(!refused && refused.error().code == eawr::sim::match_identity_invalid_code,
            "corrupt MTID digest is refused at parse time");
    }
    for (const auto version : {2U, 3U, 4U, 5U}) {
        auto legacy = start.setup;
        legacy.match_identity.reset();
        legacy.free_garrisons.clear();
        legacy.match_policy.reset();
        for (auto& unit : legacy.units) unit.garrison_enabled = true;
        if (version == 2 || version == 4) legacy.squadrons.clear();
        if (version == 4 || version == 5) legacy.queue_identities = true;
        const auto bytes = tactical::write_replay({legacy, 8, {}});
        const auto read = bytes ? tactical::parse_replay(bytes.value())
            : eawr::core::Result<tactical::TacticalReplay>::failure(bytes.error());
        expect(bytes && tactical::peek_replay_format_version(bytes.value()) == version
            && read && read.value().setup == legacy, "old v2/v3/v4/v5 replays still load unchanged");
    }
    std::optional<tactical::TacticalReplay> first_record;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto session = tactical::TacticalSession::create(recorded_setup);
        eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(static_cast<bool>(session), "identity recording session starts");
        if (!session) continue;
        for (int tick = 0; tick < 8; ++tick)
            expect(static_cast<bool>(session.value().step(executor)), "identity session advances");
        auto record = session.value().record();
        expect(record.setup.match_identity == identity, "identity is stable on 1/2/4/8 workers");
        if (!first_record) first_record = record;
        expect(record == *first_record, "identity replay agrees on 1/2/4/8 workers");
    }
    {
        const auto directory = std::filesystem::temp_directory_path()
            / ("eawr-match-identity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory / "scripts");
        std::filesystem::create_directories(directory / "xml");
        const auto write = [&](const char* path, const char* text) { std::ofstream(directory / path, std::ios::binary) << text; };
        write("scripts/test.lua", "return 1");
        write("xml/test.xml", "<AI><Value>1</Value></AI>");
        const eawr::vfs::MountSpec mount{.layer_id = "identity-test", .data_root = directory,
            .loose_logical_prefix = "data", .active_archives = {}};
        const auto read = [&]() {
            auto files = eawr::vfs::Vfs::mount(std::span<const eawr::vfs::MountSpec>(&mount, 1));
            return files ? skirmish::read_data_identity(files.value())
                : eawr::core::Result<skirmish::DataIdentity>::failure(files.error());
        };
        const auto base = read();
        write("scripts/test.lua", "return 2");
        const auto script = read();
        write("scripts/test.lua", "return 1");
        write("xml/test.xml", "<AI><Value>2</Value></AI>");
        const auto ai = read();
        expect(base && script && ai
            && base.value().script_manifest_digest != script.value().script_manifest_digest
            && base.value().ai_data_digest == script.value().ai_data_digest
            && base.value().script_manifest_digest == ai.value().script_manifest_digest
            && base.value().ai_data_digest != ai.value().ai_data_digest,
            "one Lua byte and one AI XML value change their respective effective data domains");
        if (base && script && ai) {
            auto selected = inputs;
            selected.script_manifest_digest = base.value().script_manifest_digest;
            selected.ai_data_digest = base.value().ai_data_digest;
            const auto source_identity = skirmish::match_identity(selected);
            selected.script_manifest_digest = script.value().script_manifest_digest;
            const auto script_identity = skirmish::match_identity(selected);
            selected.script_manifest_digest = base.value().script_manifest_digest;
            selected.ai_data_digest = ai.value().ai_data_digest;
            const auto ai_identity = skirmish::match_identity(selected);
            const auto script_mismatch = eawr::sim::describe_mismatch(source_identity, script_identity);
            const auto ai_mismatch = eawr::sim::describe_mismatch(source_identity, ai_identity);
            expect(script_mismatch && script_mismatch->message.find("script-manifest") != std::string::npos
                && ai_mismatch && ai_mismatch->message.find("ai-data") != std::string::npos,
                "actual source-byte changes flip and name the right match domains");
        }
        std::filesystem::remove_all(directory);
    }

    auto garrison_inputs = inputs;
    for (auto& faction : garrison_inputs.faction_forces) faction.garrison_delay_frames = 600;
    const auto registered = skirmish::build_start(fixture, garrison_inputs);
    expect(registered && registered.value().setup.free_garrisons.size() == fixture.slots.size(),
        "FL-14: faction delays bind only lobby players' free starting forces");
    if (registered) for (const auto& binding : registered.value().setup.free_garrisons) {
        expect(binding.delay_frames == 600 && !binding.templates.empty() && !binding.registered.empty(),
            "FL-14: recorded binding retains faction delay, templates and actual objects");
        for (const auto id : binding.registered) {
            const auto unit = std::find_if(registered.value().units.begin(), registered.value().units.end(),
                [id](const auto& value) { return value.state.entity_id == id; });
            expect(unit != registered.value().units.end() && unit->state.owner == binding.player
                && (unit->role == skirmish::UnitRole::craft || (unit->role == skirmish::UnitRole::free_unit && unit->craft == 0)),
                "FL-14: registered objects are free craft/generic companies, never containers or fixture ships");
        }
    }
    auto without_free = fixture;
    without_free.match = start.match;
    without_free.match->free_starting_units = false;
    const auto disabled_free = skirmish::build_start(without_free, garrison_inputs);
    expect(disabled_free && disabled_free.value().setup.free_garrisons.empty(),
        "FL-14: free-starting-units off cannot schedule replenishment");

    // WSS-35: use content for all factions, including modded names; humans and
    // non-lobby players do not acquire controllers from a faction's Basic_AI.
    auto ai_start = start;
    auto ai_inputs = inputs;
    ai_start.players[2].faction = "uNdErWoRlD";
    ai_inputs.factions.push_back({"Underworld", true, false, false, std::nullopt, "AI_Player_Underworld"});
    auto ai = skirmish::ai_setup(ai_start, ai_inputs, tables);
    // WNO-13: forwarding preserves every independent authored fog-state combination.
    for (unsigned flags = 0; flags != 4; ++flags) {
        auto fog_tables = tables;
        fog_tables.units.front().initial_state_visible_under_fow = (flags & 1U) != 0;
        fog_tables.units.front().last_state_visible_under_fow = (flags & 2U) != 0;
        const auto fog_ai = skirmish::ai_setup(ai_start, ai_inputs, fog_tables);
        const auto fog_type = std::find_if(fog_ai.content.types.begin(), fog_ai.content.types.end(), [&](const auto& type) {
            return type.type_id == skirmish::type_id(fog_tables.units.front().id);
        });
        expect(fog_type != fog_ai.content.types.end()
            && fog_type->initial_state_visible_under_fow == ((flags & 1U) != 0)
            && fog_type->last_state_visible_under_fow == ((flags & 2U) != 0),
            "WNO-13: authored initial/last fog-state flags reach typed AI data unchanged");
    }
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
        expect(rebel.combat_power_launches == Fixed{}, "FL-13: skirmish stations contribute no authored garrison power");
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

    expect(start.launches.empty(), "FL-13: skirmish stations have no authored garrison launches");
    for (const auto& unit : start.units) {
        expect(unit.state.garrison_enabled == (unit.role != skirmish::UnitRole::station),
            "FL-13: only marker-created skirmish stations disable garrison spawning");
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
    auto fog_tables = tables;
    auto& fog_type = fog_tables.units.front();
    fog_type.multisample_fow = true;
    fog_type.dense_fow_multiplier = Fixed::from_raw(Fixed::scale / 5);
    fog_type.scale_factor = whole(2);
    fog_type.footprint.custom_hard_x = whole(40);
    fog_type.footprint.collision_x = whole(100);
    fog_type.footprint.collision_y = whole(300);
    fog_type.fog_box_offset = {whole(5), whole(-7), Fixed{}};
    fog_type.collision = eawr::units::CollisionBounds{{whole(-100), whole(-200), Fixed{}},
        {whole(100), whole(400), Fixed{}}};
    const auto fog_sensors = skirmish::sensor_table(fog_tables);
    const auto fog_sensor = std::find_if(fog_sensors.begin(), fog_sensors.end(), [&](const auto& profile) {
        return profile.type_id == skirmish::type_id(fog_type.id);
    });
    expect(fog_sensor != fog_sensors.end() && fog_sensor->multisample
        && fog_sensor->dense_multiplier == fog_type.dense_fow_multiplier
        && fog_sensor->half_extents == eawr::sim::math::Vec2{whole(80), whole(600)}
        && fog_sensor->box_offset == eawr::sim::math::Vec2{whole(5), whole(-7)}
        && fog_sensor->flash_offset == Vec3{Fixed{}, whole(100), Fixed{}}
        && fog_sensor->flash_radius == whole(300), "V-21/V-19: hard sample extents and model flash bounds stay distinct");
    eawr::units::ObstacleType fog_obstacle;
    fog_obstacle.id = "Dense_Hazard";
    fog_obstacle.scale_factor = whole(99);
    fog_obstacle.footprint.space_obstacle = true;
    fog_obstacle.footprint.obstacle_radius = whole(400);
    fog_obstacle.footprint.custom_soft_radius = whole(900);
    fog_obstacle.footprint.hazard.nebula = true;
    fog_obstacle.footprint.hazard.obstacle_offset = {whole(100), whole(-200), Fixed{}};
    fog_tables.obstacles.push_back(fog_obstacle);
    auto dense_inputs = synthetic_inputs(fog_tables);
    dense_inputs.placements.clear();
    dense_inputs.map_extents = std::pair<std::uint32_t, std::uint32_t>{0x464b2000U, 0x464b2000U};
    skirmish::MapPlacement dense_placement;
    dense_placement.type = fog_obstacle.id;
    dense_placement.position = Vec3{whole(300), whole(400), Fixed{}};
    dense_inputs.placements.push_back(dense_placement);
    const auto dense_rules = skirmish::fog_rules(dense_inputs);
    expect(dense_rules && dense_rules.value() && dense_rules.value()->dense_circles.size() == 1
        && dense_rules.value()->dense_circles.front().centre == Vec3{whole(400), whole(200), Fixed{}}
        && dense_rules.value()->dense_circles.front().radius == whole(400),
        "V-22/WHZ-09: dense map circle uses raw offset/radius, ignoring scale and soft radius");
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
        expect(named.value().find("\"simulated\": false") == std::string::npos, "disabled station garrisons are absent from the census");
        expect(named.value().find("{\"record\": 8, \"type\": \"Box\", \"faction\": \"Empire\", \"reason\": \"playable_faction\"}")
                   != std::string::npos,
               "the census lists the deleted map objects");
    }
}


} // namespace skirmish_start_test_support
