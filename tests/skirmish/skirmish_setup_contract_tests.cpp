#include "skirmish_start_support.hpp"
#include "setup_map_reference.hpp"
#include "setup_map_golden.hpp"
#include "../../src/skirmish/setup_workers.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/assets/assets.hpp"
#include "eawr/assets/map.hpp"
#include "eawr/sim/commands.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <ios>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace skirmish_start_test_support {
void setup_contract(const std::filesystem::path& fixtures) {
    // WSS-25/52: chosen cash reaches all playable factions without an AI income multiplier.
    {
        auto tables = synthetic_tables();
        for (const auto* tag : {"MP_Default_Credits", "Tactical_Build_Time_Multiplier", "Space_Elevated_Vulnerability_Factor",
            "Space_Elevated_Vulnerability_Duration", "Space_Reinforcement_Collision_Check_Distance"})
            tables.constants.scalars.push_back({tag, whole(std::string_view(tag) == "MP_Default_Credits" ? 6000
                : std::string_view(tag) == "Space_Elevated_Vulnerability_Factor" ? -1 : 1), {}});
        tables.ai_credit_multiplier = whole(2);
        auto inputs = synthetic_inputs(tables);
        inputs.match_defaults.credits = whole(6000);
        for (auto& faction : inputs.factions) faction.space_unit_cap = 25;
        for (const bool free_units : {false, true}) for (const auto credits : {2750, 4750, 11750, 6000}) {
            auto fixture = synthetic_fixture();
            fixture.match = inputs.match_defaults;
            fixture.match->credits = whole(credits);
            fixture.match->free_starting_units = free_units;
            const auto start = skirmish::build_start(fixture, inputs);
            expect(static_cast<bool>(start), "WSS-52: selected-credit fixture starts with either roster policy");
            if (!start) continue;
            const auto rules = skirmish::economy_rules(start.value(), inputs, tables);
            expect(rules && rules.value().players.size() == fixture.slots.size(), "WSS-52: every occupied slot has an account");
            if (!rules) continue;
            for (const auto& player : start.value().players) if (player.lobby)
                expect(player.credits == credits && rules.value().player(player.player.player_id)
                    && rules.value().player(player.player.player_id)->credits == whole(credits),
                    "WSS-52: selected cash is independent of faction, team station, free forces and AI income scaling");
            auto setup = skirmish::recording_setup(fixture, start.value());
            const auto encoded = tactical::write_replay({setup, 30, {}});
            const auto decoded = encoded ? tactical::parse_replay(encoded.value())
                : eawr::core::Result<tactical::TacticalReplay>::failure(encoded.error());
            expect(decoded && decoded.value().setup.skirmish && decoded.value().setup.skirmish->match.credits == whole(credits),
                "WSS-52: the replay header records the selected amount exactly");
            if (decoded) {
                const auto rebound_fixture = skirmish::replay_fixture(fixture, inputs, decoded.value().setup);
                const auto rebound = rebound_fixture ? skirmish::build_start(rebound_fixture.value(), inputs)
                    : eawr::core::Result<skirmish::SkirmishStart>::failure(rebound_fixture.error());
                const auto replay_rules = rebound ? skirmish::economy_rules(rebound.value(), inputs, tables)
                    : eawr::core::Result<tactical::EconomyRules>::failure(rebound.error());
                expect(replay_rules && replay_rules.value() == rules.value(), "WSS-52: replay reconstructs the same initial accounts");
            }
            if (credits == 6000 && free_units) {
                // Identity check against the former table-default grant, with the same recorded setup.
                auto previous = rules.value();
                for (auto& player : previous.players) player.credits = whole(6000);
                auto before = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, previous);
                auto after = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, rules.value());
                expect(before && after, "WSS-52: both default-grant sessions start");
                if (!before || !after) continue;
                const eawr::sim::InlineExecutor executor;
                for (unsigned frame = 0; frame < 8; ++frame) {
                    expect(before.value().step(executor) && after.value().step(executor), "WSS-52: default identity ticks");
                    expect(before.value().state_sha256() == after.value().state_sha256(), "WSS-52: default sim hashes stay identical");
                }
                const auto old_bytes = tactical::write_replay(before.value().record());
                const auto new_bytes = tactical::write_replay(after.value().record());
                expect(old_bytes && new_bytes && old_bytes.value() == new_bytes.value(), "WSS-52: default replay bytes stay identical");
            }
        }
    }
    {
        std::vector<unsigned> visits(13);
        unsigned launches{};
        const auto worker_errors = eawr::skirmish::detail::run_setup_map_workers(visits.size(), 8,
            [&](const std::size_t index) {
                ++visits[index];
                if (index == 2 || index == 11) throw std::runtime_error(std::to_string(index));
            }, [&](std::function<void()> work) {
                if (++launches == 2) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
                return std::async(std::launch::async, std::move(work));
            });
        expect(std::all_of(visits.begin(), visits.end(), [](const auto n) { return n == 1; }),
            "thread-launch fallback and work exceptions leave no record unprocessed");
        const auto first = std::find_if(worker_errors.begin(), worker_errors.end(), [](const auto& error) { return error != nullptr; });
        expect(first == worker_errors.begin() + 2 && worker_errors[11] != nullptr,
            "worker errors are retained in record order independently of scheduling");
        if (first != worker_errors.end()) {
            try { std::rethrow_exception(*first); }
            catch (const std::runtime_error& error) { expect(std::string_view(error.what()) == "2", "first ordered error propagates"); }
        }
    }
    {
        const auto directory = std::filesystem::temp_directory_path()
            / ("eawr-setup-team-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        setup_map_golden::write_team_corpus(directory);
        const eawr::vfs::MountSpec mount{.layer_id="synthetic-team",.data_root=directory,.loose_logical_prefix="data",.active_archives={}};
        auto fs=eawr::vfs::Vfs::mount(std::span(&mount,1));
        expect(static_cast<bool>(fs), "synthetic team map mounts");
        if (fs) {
            const eawr::data::Catalog empty;
            auto catalog=eawr::data::load_catalog(fs.value(),eawr::data::Profile::foc);
            expect(static_cast<bool>(catalog), "synthetic marker catalog loads");
            for (const bool populated : {false,true}) {
                if (populated && !catalog) continue;
                const auto& selected=populated?catalog.value().catalog:empty;
                const auto golden=setup_map_golden::read(fixtures/(populated?"setup-map-eager-team.sha256":"setup-map-eager-empty-catalog.sha256"));
                expect(golden.size()==2, "pre-change synthetic list and fixture/input hashes are available");
                for (const unsigned workers : {1U,2U,4U,8U}) {
                    eawr::core::load_profile::Recording profile;
                    auto* previous=eawr::core::load_profile::active;
                    eawr::core::load_profile::active=&profile;
                    const auto maps=skirmish::setup_maps(fs.value(),selected,{},workers);
                    eawr::core::load_profile::active=previous;
                    expect(profile.costs[static_cast<std::size_t>(eawr::core::load_profile::Phase::map)].calls==1,
                        "parallel picker retains one caller-thread map-profile phase");
                    const auto list=golden.find("@maps");
                    expect(maps && list!=golden.end() && setup_map_golden::map_digest(maps.value())==list->second.map,
                        "synthetic map bytes match pre-change code on every worker count");
                    if (!maps) continue;
                    for (const auto& map:maps.value()) {
                        const auto row=golden.find(map.path);
                        expect(row!=golden.end() && setup_map_golden::row(map,fs.value(),selected)==row->second,
                            "synthetic chosen fixture, full inputs and diagnostic bytes match frozen pre-change hashes");
                        if (populated) expect(map.unavailable.empty() && map.teams.size()==2
                            && map.teams[0].spawn_capacity==1U && map.teams[1].spawn_capacity==1U
                            && map.teams[0].factions==std::vector<std::string>{"Rebel","Empire"},
                            "synthetic station/spawn records expose both supported authored teams");
                    }
                }
            }
            if (catalog) for (const auto capacity : {9U, 8U}) {
                setup_map_golden::write_team_corpus(directory, capacity);
                auto network_fs = eawr::vfs::Vfs::mount(std::span(&mount, 1));
                expect(static_cast<bool>(network_fs), "synthetic network fixture map mounts");
                if (!network_fs) continue;
                skirmish::FixtureOptions network_options;
                network_options.map = "data/art/maps/team_contract.ted";
                network_options.slots.emplace();
                for (std::uint32_t owner = 1; owner <= 9; ++owner)
                    network_options.slots->push_back({owner, "Rebel", owner <= 5 ? 0U : 1U, true, {}, owner - 1});
                const auto fixture = skirmish::fixture_from_options(network_options, network_fs.value(), catalog.value().catalog);
                expect(capacity == 9 ? fixture && fixture.value().slots.size() == 9 : !fixture,
                    "network fixture handoff accepts nine owners only within authored capacity");
                network_options.slots->push_back({10, "Rebel", 1, true, {}, 0});
                expect(!skirmish::fixture_from_options(network_options, network_fs.value(), catalog.value().catalog),
                    "network fixture handoff refuses a tenth owner");
            }
        }
        std::filesystem::remove_all(directory);
    }
    // R-SETUP-01: a nine-position local map carries every occupied seat into battle startup.
    {
        const auto tables = synthetic_tables();
        auto inputs = synthetic_inputs(tables);
        for (std::uint32_t colour = static_cast<std::uint32_t>(inputs.lobby_colours.size()); colour < 9; ++colour)
            inputs.lobby_colours.push_back({"Test_Colour_" + std::to_string(colour), {10, 11, 12}});
        skirmish::SetupMap map;
        map.path = inputs.map;
        map.metadata.capacity = 9;
        map.metadata.new_markers = true;
        map.metadata.start_positions = std::vector<eawr::assets::Vec2f>(9);
        map.teams = {{0, {"Rebel"}}, {1, {"Empire"}}};
        skirmish::SetupSelection selection;
        selection.map = map.path;
        for (std::uint32_t row = 3; row <= 9; ++row)
            selection.slots.push_back({row, "Empire", 1, false, {}});
        const std::array maps{map};
        expect(skirmish::setup_row_count(map) == 9, "nine-position map offers nine local rows");
        const auto options = skirmish::setup_options(selection, maps);
        expect(options && options.value().slots->size() == 9, "nine occupied local rows produce nine start slots");
        if (options) {
            auto fixture = synthetic_fixture();
            fixture.slots = *options.value().slots;
            const auto start = skirmish::build_start(fixture, inputs);
            expect(start && std::count_if(start.value().players.begin(), start.value().players.end(),
                [](const auto& player) { return player.lobby; }) == 9,
                "local battle startup retains all nine lobby seats");
        }
        selection.slots.push_back({10, "Empire", 1, false, {}});
        expect(!skirmish::setup_options(selection, maps), "local start rejects an unauthored tenth seat");
    }
    // WSS-54: six-player capacity is independent of three spawn markers per team.
    {
        auto tables = synthetic_tables();
        for (const auto* tag : {"MP_Default_Credits", "Tactical_Build_Time_Multiplier", "Space_Elevated_Vulnerability_Factor",
            "Space_Elevated_Vulnerability_Duration", "Space_Reinforcement_Collision_Check_Distance"})
            tables.constants.scalars.push_back({tag, whole(std::string_view(tag) == "MP_Default_Credits" ? 1000
                : std::string_view(tag) == "Space_Elevated_Vulnerability_Factor" ? -1 : 1), {}});
        tables.ai_credit_multiplier = whole(1);
        auto inputs = synthetic_inputs(tables);
        inputs.match_defaults.credits = whole(4750);
        inputs.placements.pop_back();
        inputs.placements.push_back(marker(15, "Team_00_Spawn_Point_Marker", at(50, 60), 90));
        inputs.placements.push_back(marker(16, "Team_01_Spawn_Point_Marker", at(-30, -40), 225));
        inputs.placements.push_back(marker(17, "Team_01_Spawn_Point_Marker", at(-50, -60), 270));
        for (auto& faction : inputs.factions) faction.space_unit_cap = 25;
        for (auto& forces : inputs.faction_forces) forces.garrison_delay_frames = 600;
        for (std::uint32_t colour = 3; colour < 6; ++colour)
            inputs.lobby_colours.push_back({"Test_Colour_" + std::to_string(colour), {10, 11, 12}});
        skirmish::SetupMap map;
        map.path = inputs.map;
        map.metadata.capacity = 6;
        map.metadata.new_markers = true;
        map.metadata.start_positions = std::vector<eawr::assets::Vec2f>(2);
        map.teams = {{0, {"Rebel", "Empire"}, 3}, {1, {"Rebel", "Empire"}, 3}};
        const std::array maps{map};
        for (const std::uint32_t first_team_players : {2U, 4U}) {
            skirmish::SetupSelection selection;
            selection.map = map.path;
            selection.slots.clear();
            for (std::uint32_t row = 1; row <= 6; ++row) {
                const bool rebel = row <= first_team_players;
                selection.slots.push_back({row, rebel ? "Rebel" : "Empire", rebel ? 0U : 1U, row == 1, {}});
            }
            const auto options = skirmish::setup_options(selection, maps);
            expect(static_cast<bool>(options), "WSS-54: six-player lobby accepts both 2v4 and 4v2");
            if (!options) continue;
            auto fixture = synthetic_fixture();
            fixture.slots = *options.value().slots;
            const auto start = skirmish::build_start(fixture, inputs);
            expect(static_cast<bool>(start), "WSS-54: uneven six-player battle builds");
            if (!start) continue;
            const auto overflow_player = first_team_players == 2 ? 6U : 4U;
            expect(std::count_if(start.value().players.begin(), start.value().players.end(),
                [](const auto& player) { return player.lobby; }) == 6,
                "WSS-54: all six occupied players survive startup");
            expect(std::count_if(start.value().units.begin(), start.value().units.end(),
                [](const auto& unit) { return unit.role == skirmish::UnitRole::station; }) == 2,
                "WSS-54/61: overflow does not duplicate shared stations");
            expect(std::none_of(start.value().units.begin(), start.value().units.end(),
                [&](const auto& unit) { return unit.state.owner == overflow_player; })
                && std::none_of(start.value().markers.begin(), start.value().markers.end(),
                    [&](const auto& marker) { return marker.player == overflow_player; })
                && std::none_of(start.value().setup.free_garrisons.begin(), start.value().setup.free_garrisons.end(),
                    [&](const auto& binding) { return binding.player == overflow_player; }),
                "WSS-54: overflow has no fleet, marker or free-garrison binding");
            for (const auto& slot : fixture.slots) {
                if (slot.slot == overflow_player) continue;
                expect(std::count_if(start.value().markers.begin(), start.value().markers.end(), [&](const auto& marker) {
                    return marker.use == skirmish::MarkerUse::spawn && marker.player == slot.slot;
                }) == 1, "WSS-54: each supported teammate retains one distinct spawn");
            }
            const auto economy = skirmish::economy_rules(start.value(), inputs, tables);
            expect(economy && economy.value().players.size() == 6,
                "WSS-54: overflow retains its economy account");
            if (!economy) continue;
            const auto* account = economy.value().player(overflow_player);
            expect(account && account->credits == whole(4750) && account->population_cap == 25
                && account->reinforcement_yaw == Fixed{}, "WSS-54: overflow retains credits, cap and default facing");
            expect(std::all_of(economy.value().players.begin(), economy.value().players.end(), [](const auto& player) {
                return player.credits == whole(4750);
            }), "WSS-52: every human and AI receives selected cash, including overflow and shared-station teammates");
            std::string expected_hash;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                auto session = tactical::TacticalSession::create(start.value().setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy.value());
                expect(static_cast<bool>(session), "WSS-54: uneven lobby activates a six-ledger session");
                if (!session) continue;
                expect(session.value().ledgers().size() == 6, "WSS-54: every player has a ledger");
                eawr::platform::ThreadWorkerAdapter pool(workers);
                for (int tick = 0; tick < 8; ++tick) expect(static_cast<bool>(session.value().step(pool)), "WSS-54: uneven session ticks");
                if (expected_hash.empty()) expected_hash = session.value().state_sha256();
                expect(session.value().state_sha256() == expected_hash, "WSS-54: uneven start agrees on 1/2/4/8 workers");
            }
        }
    }
    // WSS-54: modded station-only teams are admitted; all their members overflow.
    {
        auto tables = synthetic_tables();
        for (const auto* tag : {"MP_Default_Credits", "Tactical_Build_Time_Multiplier", "Space_Elevated_Vulnerability_Factor",
            "Space_Elevated_Vulnerability_Duration", "Space_Reinforcement_Collision_Check_Distance"})
            tables.constants.scalars.push_back({tag, whole(std::string_view(tag) == "MP_Default_Credits" ? 1000
                : std::string_view(tag) == "Space_Elevated_Vulnerability_Factor" ? -1 : 1), {}});
        tables.ai_credit_multiplier = whole(1);
        auto inputs = synthetic_inputs(tables);
        inputs.placements.erase(inputs.placements.begin() + 4);
        for (auto& faction : inputs.factions) faction.space_unit_cap = 25;
        auto fixture = synthetic_fixture();
        fixture.slots = {{1, "Rebel", 0, true, {}}, {2, "Empire", 1, false, {}}, {3, "Empire", 1, false, {}}};
        const auto start = skirmish::build_start(fixture, inputs);
        expect(static_cast<bool>(start), "WSS-54: a station-only team with two overflow members starts");
        if (start) {
            expect(std::count_if(start.value().units.begin(), start.value().units.end(), [](const auto& unit) {
                return unit.state.owner == 2 && unit.role == skirmish::UnitRole::station;
            }) == 1 && std::none_of(start.value().units.begin(), start.value().units.end(), [](const auto& unit) {
                return (unit.state.owner == 2 || unit.state.owner == 3) && unit.role != skirmish::UnitRole::station;
            }), "WSS-54/61: the all-overflow team retains its one shared station and no fleet");
            const auto economy = skirmish::economy_rules(start.value(), inputs, tables);
            expect(economy && economy.value().players.size() == 3
                && economy.value().player(2) && economy.value().player(3)
                && economy.value().player(2)->reinforcement_yaw == Fixed{}
                && economy.value().player(3)->reinforcement_yaw == Fixed{},
                "WSS-54: every member of a zero-spawn team keeps its independent account and project default yaw");
        }
    }
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
        const auto overflow_start = skirmish::build_start(overflow, inputs);
        expect(overflow_start && std::none_of(overflow_start.value().units.begin(), overflow_start.value().units.end(),
            [](const auto& unit) { return unit.state.owner == 4; }),
            "WSS-54: a third teammate on two spawns starts without a fleet or duplicate station");
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
            const auto path = directory / "art/maps" / name;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream file(path, std::ios::binary);
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
        write("eligible_bad_body.ted", 2, 4, false);
        { std::ofstream file(directory / "art/maps/eligible_bad_body.ted", std::ios::binary | std::ios::app); file.put('x'); }
        write("../../custommaps/loose.ted", 2, 4, true);
        write("../../custommaps/false_header.ted", 2, 6, false);
        write("../../custommaps/_mp_space_custom.ted", 2, 6, true);
        write("../../custommaps/unrelated_name.ted", 2, 6, true);
        write("../../custommaps/land.ted", 1, 4, true);
        // Original MEG-v1 fixture: two authored maps, one shadowed by loose data.
        const std::array<std::string, 2> archive_names{
            "DATA\\CUSTOMMAPS\\ARCHIVED.TED", "DATA\\CUSTOMMAPS\\LOOSE.TED"};
        std::ifstream payload_file(directory / "custommaps/loose.ted", std::ios::binary);
        const std::string payload((std::istreambuf_iterator<char>(payload_file)), std::istreambuf_iterator<char>());
        payload_file.close();
        std::ofstream archive(directory / "maps.meg", std::ios::binary);
        const auto archive_integer = [&](std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) archive.put(static_cast<char>((value >> (8U * i)) & 255U));
        };
        archive_integer(2); archive_integer(2);
        std::uint32_t offset = 8 + 40;
        for (const auto& name : archive_names) {
            archive.put(static_cast<char>(name.size())); archive.put(0);
            archive << name;
            offset += 2 + static_cast<std::uint32_t>(name.size());
        }
        for (std::uint32_t index = 0; index < 2; ++index) {
            archive_integer(0); archive_integer(index);
            archive_integer(static_cast<std::uint32_t>(payload.size()));
            archive_integer(offset); archive_integer(index);
            offset += static_cast<std::uint32_t>(payload.size());
        }
        archive << payload << payload;
        archive.close();
        // Different capacity proves the loose winner supplies decoded metadata.
        write("../../custommaps/loose.ted", 2, 8, true);
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
            "<Min_Skirmish_Credits>2500</Min_Skirmish_Credits>"
            "<Max_Skirmish_Credits>9000</Max_Skirmish_Credits>"
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
            .loose_logical_prefix = "data", .active_archives = {{directory / "maps.meg", "synthetic-maps", ""}}};
        auto filesystem = eawr::vfs::Vfs::mount(std::span<const eawr::vfs::MountSpec>(&mount, 1));
        expect(static_cast<bool>(filesystem), "synthetic map corpus mounts");
        if (filesystem) {
            const eawr::data::Catalog catalog;
            auto official = skirmish::setup_maps(filesystem.value(), catalog);
            expect(official && official.value().size() == 1
                && std::all_of(official.value().begin(), official.value().end(), [](const auto& map) {
                    return map.path.ends_with("unrelated_name.ted") && map.name == "unrelated_name" && map.metadata.custom == false;
                }), "WSS-03/06/07: custom-directory official headers do not alter the original official list");
            skirmish::SetupMapQuery custom_query; custom_query.custom = true;
            auto custom = skirmish::setup_maps(filesystem.value(), catalog, custom_query);
            expect(custom && custom.value().size() == 4,
                "WSS-07: custom tab includes art/maps, loose custommaps and MEG custommaps once each; excludes land");
            if (custom) {
                expect(std::any_of(custom.value().begin(), custom.value().end(), [](const auto& map) {
                    return map.path == "data/custommaps/unrelated_name.ted" && map.metadata.capacity == 6U;
                }), "official-directory map with the same stem does not hide a custom-tab map");
                expect(std::count_if(custom.value().begin(), custom.value().end(), [](const auto& map) {
                    return map.path.ends_with("/_mp_space_custom.ted");
                }) == 1 && std::any_of(custom.value().begin(), custom.value().end(), [](const auto& map) {
                    return map.path == "data/art/maps/_mp_space_custom.ted" && map.metadata.capacity == 4U;
                }), "same custom-map stem appears once, with the original directory path and metadata");
                const auto loose = std::find_if(custom.value().begin(), custom.value().end(), [](const auto& map) {
                    return map.path == "data/custommaps/loose.ted";
                });
                expect(loose != custom.value().end() && loose->metadata.capacity == 8U,
                    "loose custom map wins over the same archived path");
                expect(std::any_of(custom.value().begin(), custom.value().end(), [](const auto& map) {
                    return map.path == "data/custommaps/archived.ted";
                }), "archive-only custom map is discovered through the mounted VFS");
            }
            for (const auto* list : {&official, &custom}) {
                expect(*list && std::none_of(list->value().begin(), list->value().end(), [](const auto& map) {
                    return map.path == "data/custommaps/false_header.ted";
                }), "false-header custom-directory map is absent by path from both tabs");
            }
            skirmish::SetupMapQuery all_query; all_query.custom.reset();
            const auto all = skirmish::setup_maps(filesystem.value(), catalog, all_query);
            expect(all && all.value().size() == 5, "shared local/LAN catalogue retains both tab classes with same-stem separation");
            for (const auto& query : {skirmish::SetupMapQuery{}, custom_query, all_query}) {
                const auto eager = setup_map_reference::eager_maps(filesystem.value(), catalog, query);
                expect(static_cast<bool>(eager), "eager synthetic setup reference succeeds");
                for (const unsigned workers : {1U, 2U, 4U, 8U}) {
                    const auto parallel = skirmish::setup_maps(filesystem.value(), catalog, query, workers);
                    if (parallel) {
                        auto stock = parallel.value();
                        std::erase_if(stock, [](const auto& map) { return !map.path.starts_with("data/art/maps/"); });
                        expect(eager && setup_map_reference::bytes(eager.value()) == setup_map_reference::bytes(stock),
                            "original directory map bytes and diagnostics equal frozen eager reference at every worker count");
                    }
                    const auto& expected = !query.custom ? all : (query.custom == true ? custom : official);
                    expect(expected && parallel && setup_map_reference::bytes(expected.value()) == setup_map_reference::bytes(parallel.value()),
                        "combined map catalogue is identical on 1/2/4/8 workers");
                }
            }
            auto shared_fs = std::make_shared<const eawr::vfs::Vfs>(std::move(filesystem).value());
            auto shared_catalog = std::make_shared<const eawr::data::Catalog>();
            skirmish::SetupMapCache cache;
            expect(static_cast<bool>(cache.load(shared_fs, shared_catalog, {})), "setup cache builds loaded snapshot");
            const auto* cached = cache.find(*shared_fs, *shared_catalog, {});
            const auto shown = skirmish::setup_domain_maps(*shared_fs, *shared_catalog, {}, skirmish::land_skirmish_enabled);
            expect(cached && shown && setup_map_reference::bytes(*cached) == setup_map_reference::bytes(shown.value()), "setup and lobby share identical map bytes");
            expect(!cache.find(*shared_fs, *shared_catalog, custom_query), "cache rejects different map query");
            auto remounted = eawr::vfs::Vfs::mount(std::span<const eawr::vfs::MountSpec>(&mount, 1));
            expect(remounted && !cache.find(remounted.value(), *shared_catalog, {}), "cache rejects remounted effective content");
            const eawr::data::Catalog replacement_catalog;
            expect(!cache.find(*shared_fs, replacement_catalog, {}), "cache rejects replacement catalog");
            // Keep the rest of this existing corpus on its original mounted view.
            filesystem = eawr::vfs::Vfs::mount(std::span<const eawr::vfs::MountSpec>(&mount, 1));
            const auto defaults = skirmish::read_match_defaults(filesystem.value());
            expect(defaults && !defaults.value().allow_heroes && !defaults.value().free_starting_units
                && defaults.value().allow_superweapons && defaults.value().pre_built_base
                && defaults.value().credits == whole(7000) && defaults.value().game_timer == 23
                && defaults.value().start_tech == 2 && defaults.value().max_tech == 7 && defaults.value().win_integer == -4
                && defaults.value().win_float == Fixed::from_raw(Fixed::scale * 5 / 2) && defaults.value().auto_resolve == 2,
                "WSS-22: complete copied defaults come from authored data, including hidden fields");
            const auto bounds = skirmish::read_credits_bounds(filesystem.value());
            expect(bounds && bounds.value().minimum == 2500 && bounds.value().maximum == 9000,
                "WSS-25: the credits slider bounds come from the authored constants");
            const auto palette = skirmish::read_lobby_colours(filesystem.value());
            expect(palette && palette.value().size() == 9 && palette.value()[0].constant == "MP_Color_Blue"
                && palette.value()[4].constant == "MP_Color_Cyan" && palette.value()[8].constant == "MP_Color_Eight",
                "WSS-20: selector palette is stable across XML order and ignores extra mod colours");
            auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
            expect(static_cast<bool>(loaded), "synthetic inherited policy catalog loads");
            if (loaded) {
                skirmish::FixtureOptions custom_start;
                custom_start.map = "data/custommaps/loose.ted";
                const auto custom_fixture = skirmish::fixture_from_options(custom_start, filesystem.value(), loaded.value().catalog);
                expect(!custom_fixture && custom_fixture.error().message == "Legacy map starts are not supported yet.",
                    "custom-directory fixture reaches authored start validation instead of rejecting its logical path");
                custom_start.map = "data/custommaps/land.ted";
                const auto custom_land = skirmish::fixture_from_options(custom_start, filesystem.value(), loaded.value().catalog);
                expect(!custom_land && custom_land.error().message == "skirmish map must be a space map",
                    "custom-directory fixture still rejects an authored land map");
                const auto placed_land = skirmish::placed_fixture("data/custommaps/land.ted", {}, 0,
                    filesystem.value(), loaded.value().catalog);
                expect(!placed_land && placed_land.error().message == "demo map must be a space map",
                    "placed fixture shares custom-directory path admission and battle-kind validation");
                for (const auto* path : {"data/other/loose.ted", "data/custommaps/nested/loose.ted",
                    "data/custommaps/../loose.ted", "data/custommaps/loose.xml", "data/custommaps/nested\\loose.ted"}) {
                    custom_start.map = path;
                    const auto invalid = skirmish::fixture_from_options(custom_start, filesystem.value(), loaded.value().catalog);
                    expect(!invalid && invalid.error().message.starts_with("skirmish map must be a logical"),
                        "custom-directory support retains directory, extension and traversal guards");
                    const auto invalid_placed = skirmish::placed_fixture(path, {}, 0, filesystem.value(), loaded.value().catalog);
                    expect(!invalid_placed && invalid_placed.error().message.starts_with("demo map must be a logical"),
                        "placed custom-directory fixtures retain the same path guards");
                }
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
            expect(invalid && !skirmish::read_credits_bounds(invalid.value()), "WSS-25: missing slider bounds reject deterministically");
            constants("<Min_Skirmish_Credits>8000</Min_Skirmish_Credits><Max_Skirmish_Credits>2000</Max_Skirmish_Credits>");
            auto inverted = eawr::vfs::Vfs::mount(std::span<const eawr::vfs::MountSpec>(&mount, 1));
            expect(inverted && !skirmish::read_credits_bounds(inverted.value()), "WSS-25: an inverted slider range is rejected");
        }
        std::filesystem::remove_all(directory);
    }
    // WSS-12/13: the dropdown is the catalog's playable factions in data order, whatever any map offers.
    {
        const auto directory = std::filesystem::temp_directory_path()
            / ("eawr-setup-factions-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory / "xml");
        std::ofstream(directory / "xml/factionfiles.xml")
            << "<Faction_Files><File>factions.xml</File><File>factions_more.xml</File></Faction_Files>";
        std::ofstream(directory / "xml/factions.xml")
            << "<Factions>"
               "<Faction Name=\"Rebel\"><Is_Playable>Yes</Is_Playable><Text_ID>TEXT_REBEL</Text_ID></Faction>"
               "<Faction Name=\"Empire\"><Is_Playable>Yes</Is_Playable></Faction>"
               "<Faction Name=\"Pirates\"><Is_Playable>No</Is_Playable><Create_Player_In_Multiplayer_Games>Yes</Create_Player_In_Multiplayer_Games></Faction>"
               "<Faction Name=\"Alpha_Fleet\"><Is_Playable>Yes</Is_Playable><Text_ID>TEXT_ALPHA</Text_ID>"
               "<Is_Debug_Switchable_To>No</Is_Debug_Switchable_To></Faction>"
               "<Faction Name=\"Derived_Fleet\"><Variant_Of_Existing_Type>Alpha_Fleet</Variant_Of_Existing_Type></Faction>"
               "<Faction Name=\"Bare_Fleet\"><Is_Playable>Yes</Is_Playable></Faction>"
               "</Factions>";
        // A later file shadows Bare_Fleet: the resolved flag, not the first definition, decides.
        std::ofstream(directory / "xml/factions_more.xml")
            << "<Factions>"
               "<Faction Name=\"Bare_Fleet\"><Is_Playable>No</Is_Playable></Faction>"
               "<Faction Name=\"Zed_Fleet\"><Is_Playable>Yes</Is_Playable></Faction>"
               "</Factions>";
        const eawr::vfs::MountSpec mount{.layer_id = "synthetic-factions", .data_root = directory,
            .loose_logical_prefix = "data", .active_archives = {}};
        auto filesystem = eawr::vfs::Vfs::mount(std::span<const eawr::vfs::MountSpec>(&mount, 1));
        auto loaded = filesystem ? eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc)
                                 : eawr::core::Result<eawr::data::LoadResult>::failure(filesystem.error());
        expect(static_cast<bool>(loaded), "synthetic faction catalog loads");
        if (loaded) {
            const auto factions = skirmish::setup_factions(loaded.value().catalog);
            std::vector<std::string> names;
            for (const auto& faction : factions) names.push_back(faction.name);
            expect(names == std::vector<std::string>{"Rebel", "Empire", "Alpha_Fleet", "Derived_Fleet", "Zed_Fleet"},
                "WSS-12/13: playable catalog factions in data order, with no name list, count limit or debug-switch gate");
            expect(factions.size() == 5 && factions[0].text_id == "TEXT_REBEL" && factions[1].text_id.empty()
                && factions[2].text_id == "TEXT_ALPHA" && factions[3].text_id == "TEXT_ALPHA",
                "WSS-12: captions come from the resolved Text_ID, inherited when a variant omits it");
            // WSS-LAN: host and peers key the roster on the catalog ordinals of scene::faction_order.
            const auto ordinals = eawr::scene::faction_order(loaded.value().catalog);
            std::size_t previous{};
            bool ordered = true;
            for (const auto& name : names) {
                const auto at = std::find(ordinals.begin(), ordinals.end(), name);
                ordered = ordered && at != ordinals.end() && static_cast<std::size_t>(at - ordinals.begin()) >= previous;
                if (at != ordinals.end()) previous = static_cast<std::size_t>(at - ordinals.begin());
            }
            expect(ordered && ordinals.size() > names.size(), "every dropdown entry keeps its catalog ordinal, in the same order");
        }
        std::filesystem::remove_all(directory);
    }
    {
        const auto directory = std::filesystem::temp_directory_path()
            / ("eawr-playable-candidates-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        setup_map_golden::write_playable_candidate_corpus(directory);
        const eawr::vfs::MountSpec mount{.layer_id = "playable-candidates", .data_root = directory,
            .loose_logical_prefix = "data", .active_archives = {}};
        auto filesystem = eawr::vfs::Vfs::mount(std::span(&mount, 1));
        expect(static_cast<bool>(filesystem), "playable station candidate corpus mounts");
        if (filesystem) {
            auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
            expect(static_cast<bool>(loaded), "playable station candidate catalog loads");
            if (loaded) {
                const auto maps = skirmish::setup_maps(filesystem.value(), loaded.value().catalog);
                expect(maps && maps.value().size() == 1 && maps.value().front().unavailable.empty(),
                    "mixed-case and non-playable station affiliations leave a supported map");
                if (maps && maps.value().size() == 1) {
                    const auto& map = maps.value().front();
                    expect(map.teams.size() == 2 && std::all_of(map.teams.begin(), map.teams.end(), [](const auto& team) {
                        return team.factions == std::vector<std::string>{"Rebel", "Empire", "Additional_Fleet"};
                    }), "station candidates are canonical playable names, excluding non-playable and non-faction tokens");
                    skirmish::SetupSelection pick; pick.map = map.path;
                    pick.slots.front().faction = "additional_fleet";
                    expect(static_cast<bool>(skirmish::setup_options(pick, maps.value())),
                        "authored mod faction starts case-insensitively after catalog filtering");
                    for (const auto* invalid : {"Pirates", "Not_A_Faction"}) {
                        pick.slots.front().faction = invalid;
                        const auto refused = skirmish::setup_options(pick, maps.value());
                        expect(!refused && refused.error().message.starts_with("Project limit:")
                            && refused.error().message.find(invalid) != std::string::npos,
                            "a station affiliation outside the playable catalog cannot bypass local Start validation");
                    }
                }
            }
        }
        std::filesystem::remove_all(directory);
    }
    // A playable faction no station names keeps its dropdown entry; Start refuses it with the named project limit,
    // and an affiliation matches case-insensitively with any number of candidates.
    {
        skirmish::SetupMap many;
        many.path = "data/art/maps/_mp_space_many.ted";
        many.name = "Many";
        many.metadata.capacity = 2;
        many.metadata.new_markers = true;
        many.metadata.start_positions = std::vector<eawr::assets::Vec2f>(2);
        many.teams = {{0, {"Rebel", "Empire", "Underworld", "CIS", "Republic"}}, {1, {"Rebel", "Empire", "Underworld", "CIS", "Republic"}}};
        const std::array many_maps{many};
        skirmish::SetupSelection pick;
        pick.map = many.path;
        pick.slots[0].faction = "cis";
        pick.slots[1].faction = "REPUBLIC";
        pick.slots[1].team = 1;
        expect(static_cast<bool>(skirmish::setup_options(pick, many_maps)),
            "fourth and fifth station candidates start, matched case-insensitively");
        pick.slots[1].faction = "Alpha_Fleet";
        const auto refused = skirmish::setup_options(pick, many_maps);
        expect(!refused && refused.error().message.starts_with("Project limit:")
            && refused.error().message.find("Alpha_Fleet") != std::string::npos,
            "a playable faction without a station is refused at Start, by name, as a project limit");
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
    query.kind = skirmish::setup_map_kind(tactical::BattleDomain::land);
    expect(skirmish::setup_map_eligible(authored, query), "land query admits the authored land header");
    authored.kind = eawr::assets::MapKind::space;
    expect(!skirmish::setup_map_eligible(authored, query), "land query excludes the authored space header");
    query.kind = skirmish::setup_map_kind(tactical::BattleDomain::space);
    expect(skirmish::setup_map_eligible(authored, query), "space domain query preserves ordinary map eligibility");
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
    // WSS-31: the copied record's space win condition reaches the start options; text that names neither
    // evidenced condition leaves the data's default (no victory override).
    expect(!options.value().victory_condition, "no copied record leaves the data's victory default");
    auto victory = selection;
    victory.match = skirmish::MatchOptions{};
    victory.match->tactical_win_condition = "SKIRMISH_ALL_ENEMY_UNITS_DESTROYED";
    auto chosen = skirmish::setup_options(victory, maps);
    expect(chosen && chosen.value().victory_condition
        && *chosen.value().victory_condition == eawr::sim::tactical::VictoryCondition::all_enemy_units_destroyed,
        "WSS-31: all enemy units destroyed is passed to the start");
    victory.match->tactical_win_condition = "SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED";
    chosen = skirmish::setup_options(victory, maps);
    expect(chosen && chosen.value().victory_condition
        && *chosen.value().victory_condition == eawr::sim::tactical::VictoryCondition::enemy_starbase_destroyed,
        "WSS-31: the starbase condition is passed to the start");
    victory.match->tactical_win_condition = "SKIRMISH_ALL_ENEMY_BASE_COMPONENTS_DESTROYED";
    chosen = skirmish::setup_options(victory, maps);
    expect(chosen && !chosen.value().victory_condition,
        "WSS-31: a land condition in the record leaves the data's default");
    auto bad = selection;
    for (const auto level : {3U, 255U}) {
        bad = selection; bad.slots[1].difficulty = static_cast<tactical::AiLevel>(level);
        for (const bool local : {true, false}) {
            const auto invalid = skirmish::validate_setup_slots(bad.slots, map.metadata, local);
            expect(!invalid && invalid.error().message == "Choose a valid AI difficulty level.",
                "canonical local/network validation refuses out-of-range AI levels");
        }
    }
    bad = selection;
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
    auto network_roster = roster;
    network_roster.slots.push_back({9, "Rebel", 2, false, {}});
    expect(static_cast<bool>(skirmish::setup_options(network_roster, maps)), "R-SETUP-01: local controls accept the ninth AI row");
    for (auto& row : network_roster.slots) row.human = true;
    expect(static_cast<bool>(skirmish::setup_options(network_roster, maps, false)),
        "network controls accept nine separate human rows on a nine-row map");
    auto eight_rows = maps; eight_rows[0].metadata.capacity = 8;
    expect(!skirmish::setup_options(network_roster, eight_rows, false),
        "eight-row maps refuse a ninth network human");
    auto repaired = network_roster;
    skirmish::repair_setup_selection(repaired, maps[0], false);
    expect(repaired.slots.size() == 9 && skirmish::setup_row_count(maps[0], false) == 9,
        "network map repair preserves the ninth authored row");
    skirmish::repair_setup_selection(repaired, eight_rows[0], false);
    expect(repaired.slots.size() == 8, "network map repair obeys an eight-row map");
    repaired = network_roster;
    skirmish::repair_setup_selection(repaired, maps[0]);
    expect(repaired.slots.size() == 9 && skirmish::setup_row_count(maps[0]) == 9,
        "R-SETUP-01: local map repair retains the ninth authored row");
    auto limited = maps;
    limited[0].teams[2].spawn_capacity = 2;
    const auto overflow = skirmish::setup_options(roster, limited);
    expect(static_cast<bool>(overflow), "WSS-54: setup allows team spawn overflow within total map capacity");
    limited[0].teams[2].spawn_capacity = 0;
    expect(static_cast<bool>(skirmish::setup_options(roster, limited)),
        "WSS-54: zero spawn markers do not add a lobby team limit");
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
    expect(static_cast<bool>(skirmish::setup_options(roster, maps)), "R-SETUP-01: all nine local rows reach the start options");
    for (const auto capacity : {2U, 4U, 8U, 9U}) {
        auto authored_map = map; authored_map.metadata.capacity = capacity;
        expect(skirmish::setup_row_count(authored_map) == capacity, "local row count follows authored capacity");
    }
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
        policy_tables.constants.scalars.push_back({tag, whole(std::string_view(tag) == "MP_Default_Credits" ? 1000
            : std::string_view(tag) == "Space_Elevated_Vulnerability_Factor" ? -1 : 1), {}});
    auto special = unit_type("Policy_Upgrade", UnitKind::ship, "Rebel, Empire", 0);
    special.production.upgrade_object = true;
    special.special_unlocks = {{true, false, false}};
    special.consume_after_special_fire = true;
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
        expect(std::any_of(rules.value().players.begin(), rules.value().players.end(), [](const auto& player) { return player.ai; })
            && std::any_of(rules.value().players.begin(), rules.value().players.end(), [](const auto& player) { return !player.ai; }),
            "WSW-07: the production contract covers an AI and a human player");
        expect(std::all_of(rules.value().players.begin(), rules.value().players.end(), [&](const auto& player) {
            return player.credits == defaults.credits
                && player.start_tech == static_cast<std::uint32_t>(defaults.start_tech)
                && player.max_tech == static_cast<std::uint32_t>(defaults.max_tech);
        }), "WSS-52/WPR-02: selected cash and tech bind every player independently of default constants");
        auto missing_difficulty = policy_tables;
        missing_difficulty.ai_credit_multiplier.reset();
        expect(!skirmish::economy_rules(policy_start.value(), policy_inputs, missing_difficulty),
            "WPR-12: missing selected AI credit data cannot silently use a neutral multiplier");
        const auto recorded_setup = skirmish::recording_setup(policy_fixture, policy_start.value());
        expect(recorded_setup.skirmish && recorded_setup.skirmish->match.credits == defaults.credits,
            "WSS-52: recording retains selected credits independently of default constants");
        auto session = tactical::TacticalSession::create(recorded_setup, {}, {}, {}, std::nullopt, {}, {}, {}, rules.value());
        expect(static_cast<bool>(session), "policy admission session starts");
        if (!session) continue;
        std::uint32_t purchase_frames = 0;
        std::vector<tactical::PlayerId> purchase_players;
        for (const auto& unit : policy_start.value().units) {
            if (unit.role != skirmish::UnitRole::station) continue;
            const auto* menu = rules.value().menu(unit.state.type_id, policy_start.value().setup.players[unit.state.owner - 1].faction_id);
            expect(menu && (menu->find(skirmish::type_id("Cruiser")) != nullptr) == allowed
                && (menu->find(skirmish::type_id("Policy_Upgrade")) != nullptr) == allowed
                && menu->find(skirmish::type_id("Fighter_Squadron")), "WSS-29/30: policy filters listing while ordinary entries remain");
            expect(session.value().build_allowed(unit.state.owner, unit.state.entity_id, skirmish::type_id("Cruiser")) == allowed
                && session.value().build_allowed(unit.state.owner, unit.state.entity_id, skirmish::type_id("Policy_Upgrade")) == allowed,
                "WSS-29/30: authoritative eligibility applies to human and AI producers");
            if (allowed && menu) {
                const auto* option = menu->find(skirmish::type_id("Policy_Upgrade"));
                expect(option && option->available, "WSW-07: an unlock-only automatic upgrade is available in both station menus");
                if (option) purchase_frames = std::max({purchase_frames, option->build_frames, option->ai_build_frames});
                // The fixture has two stations for player one; buy one enabling object per player.
                if (std::find(purchase_players.begin(), purchase_players.end(), unit.state.owner) == purchase_players.end()) {
                    const auto submitted = session.value().submit({{0, unit.state.owner, 1}, {unit.state.entity_id},
                        tactical::BuyPayload{skirmish::type_id("Policy_Upgrade")}});
                    expect(static_cast<bool>(submitted), "WSW-07: player and AI purchases submit");
                    if (!submitted) std::cerr << submitted.error().message << '\n';
                    purchase_players.push_back(unit.state.owner);
                }
            }
        }
        std::vector<std::string> hashes;
        if (allowed) {
            expect(purchase_players.size() == rules.value().players.size(),
                "WSW-07: one producer submits the enabling purchase for each human and AI player");
            const eawr::sim::InlineExecutor executor;
            for (std::uint32_t frame = 0; frame < purchase_frames + 2; ++frame) {
                const auto tick = session.value().step(executor);
                expect(static_cast<bool>(tick), "WSW-07: player and AI unlock purchases complete");
                if (!tick) break;
                hashes.push_back(tick.value().state_sha256);
            }
            for (const auto& player : rules.value().players) {
                const auto views = session.value().snapshot()->economy();
                const auto view = std::find_if(views.begin(), views.end(), [&](const auto& account) { return account.player == player.player; });
                expect(view != views.end() && view->special_weapons_enabled && view->completed.size() == 1,
                    "WSW-07/08: player and AI economy views retain the enabling use object after purchase");
            }
        }
        const auto replay_fixture = skirmish::replay_fixture(policy_fixture, policy_inputs, recorded_setup);
        expect(replay_fixture && replay_fixture.value().slots.size() == policy_fixture.slots.size(),
            "recorded lobby players resolve the matching faction bindings");
        if (!replay_fixture) continue;
        const auto rebuilt = skirmish::build_start(replay_fixture.value(), policy_inputs);
        const auto rebound = rebuilt ? skirmish::economy_rules(rebuilt.value(), policy_inputs, policy_tables)
            : eawr::core::Result<tactical::EconomyRules>::failure(rebuilt.error());
        expect(rebound && rebound.value() == rules.value(), "recorded policy rebuilds the same purchase menus");
        if (!rebound) continue;
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
            for (std::size_t tick = 0; tick < hashes.size(); ++tick) {
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
