#include "viewer_lan_fixtures.hpp"
#include "lan_match.hpp"
#include "skirmish_prepare.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/net/lobby_session.hpp"
#include "eawr/net/setup.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace viewer = eawr::presentation::godot_backend;
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;

namespace viewer_lan_test {
void missing_planned_owner_is_refused() {
    stage = "three owners on a two-row loaded map";
    auto initial = setup();
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map; selected.match = initial.skirmish->match;
    selected.slots = std::vector<eawr::skirmish::LobbySlot>{{1, "Rebel", 1, true, {}, 0}, {2, "Empire", 2, true, {}, 1}};
    eawr::skirmish::SetupMap map; map.path = *selected.map;
    map.metadata.capacity = 2; map.metadata.new_markers = true; map.metadata.start_positions.emplace(3);
    map.teams = {{1, {"Rebel"}, 1}, {2, {"Empire"}, 1}};
    auto host = make_match(); host->configure_seats({1, 2, 3});
    host->configure_lobby("H_host", 0, {map}, [initial](const auto&) {
        return eawr::core::Result<tactical::TacticalSetup>::success(initial);
    });
    host->host(0); host->pump_setup(selected);
    expect(host->view().phase == viewer::LanPhase::ended
        && host->view().diagnostic_code == net::lobby_diagnostic(net::LobbyReason::invalid_choice, "").code,
        "missing planned third owner is a typed refusal before any room or partial roster can start");
    host->cancel();
}

void nine_seat_staging(const std::vector<tactical::PlayerId>& owners) {
    stage = "nine-seat loaded-map production staging";
    const auto rows = std::max(3U, *std::max_element(owners.begin(), owners.end()));
    auto initial = human_setup(rows);
    const std::vector<std::string> factions{"H_mod_A", "H_mod_B", "H_mod_C"};
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map; selected.match = initial.skirmish->match; selected.seed = initial.seed;
    selected.slots.emplace();
    eawr::skirmish::SetupMap map; map.path = *selected.map;
    map.metadata.capacity = rows; map.metadata.new_markers = true; map.metadata.start_positions.emplace(rows + 1);
    initial.departure_policy.emplace(); initial.queue_identities = true;
    for (unsigned row = 0; row < rows; ++row) {
        const bool human = std::find(owners.begin(), owners.end(), row + 1) != owners.end();
        const auto& faction = factions[row % factions.size()];
        selected.slots->push_back({row + 1, faction, row + 1, human, {}, row});
        map.teams.push_back({row + 1, {faction}, 1});
        initial.players[row].faction_id = eawr::skirmish::faction_id(faction);
        initial.players[row].team_id = row + 1;
        initial.skirmish->slots[row].human = human;
        if (human) initial.departure_policy->humans.push_back(row + 1);
    }
    const viewer::LanLobbyBuilder builder = [initial](const auto&) {
        return eawr::core::Result<tactical::TacticalSetup>::success(initial);
    };
    std::vector<std::shared_ptr<viewer::LanMatch>> matches;
    for (unsigned peer = 0; peer < owners.size(); ++peer) {
        auto match = make_match(); match->configure_factions(factions); match->configure_seats(owners);
        match->configure_lobby("H_guest_" + std::to_string(peer), peer % 3, {map}, builder);
        matches.push_back(std::move(match));
    }
    auto& host = *matches.front(); host.host(0); host.pump_setup(selected);
    for (unsigned peer = 1; peer < matches.size(); ++peer)
        matches[peer]->join("127.0.0.1:" + std::to_string(host.view().port));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const auto pump = [&] { for (auto& match : matches) match->pump_setup(selected); };
    std::string milestone = "admission and identity";
    const auto wait = [&](const auto& predicate) {
        stage = "production staging (" + std::to_string(owners.size()) + " seats): " + milestone;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                for (std::size_t peer = 0; peer < matches.size(); ++peer) {
                    const auto view = matches[peer]->view(); const auto state = matches[peer]->lobby_state();
                    std::cerr << "peer " << peer << " phase=" << static_cast<int>(view.phase) << " code=" << view.diagnostic_code;
                    if (state) {
                        std::cerr << " revision=" << state->snapshot.revision << " seats=" << state->snapshot.seats.size()
                            << " identity_pending=" << state->identity_pending << " preparing=" << state->preparing << " ready=" << state->local_ready;
                        for (const auto& seat : state->snapshot.seats) std::cerr << " [" << seat.peer << ':' << seat.selection.name << ':' << seat.selection.slot << ':' << seat.selection.colour << ']';
                        if (state->notice) std::cerr << " notice=" << static_cast<int>(state->notice->reason) << ':' << state->notice->message;
                    }
                    std::cerr << '\n';
                }
                expect(false, "nine-seat staging deadline");
            }
            pump(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    wait([&] { return host.lobby_state() && host.lobby_state()->snapshot.seats.size() == owners.size()
        && std::all_of(matches.begin(), matches.end(), [&](const auto& match) {
            return match->lobby_state() && match->lobby_state()->snapshot == host.lobby_state()->snapshot
                && !match->lobby_state()->identity_pending;
        }); });
    expect(host.lobby_state()->snapshot.capacity == owners.size(), "production staging preserves requested command capacity");
    const auto admitted_state = host.lobby_state();
    std::multiset<tactical::PlayerId> admitted;
    for (const auto& seat : admitted_state->snapshot.seats) admitted.insert(seat.player);
    expect(admitted == std::multiset<tactical::PlayerId>(owners.begin(), owners.end()), "loaded owner assignments survive admission");
    if (std::find(owners.begin(), owners.end(), 3U) == owners.end()) {
        const auto before = host.lobby_state()->snapshot;
        auto& guest = *matches.back();
        auto choice = std::find_if(before.seats.begin(), before.seats.end(), [&](const auto& seat) { return seat.peer == guest.view().seat; })->selection;
        choice.slot = 3;
        expect(guest.lobby_select(choice), "crafted guest AI-row select reaches the host validator");
        milestone = "AI row refusal";
        wait([&] { return guest.lobby_state()->notice && guest.lobby_state()->notice->reason == net::LobbyReason::invalid_choice; });
        expect(host.lobby_state()->snapshot == before, "host refuses guest takeover of an unplanned AI row without a revision change");
    }
    for (auto& match : matches) expect(match->lobby_ready(true), "all command seats explicitly ready");
    milestone = "all controllers ready";
    wait([&] { return host.view().start_enabled; });
    auto invalid = selected; invalid.slots->push_back({rows + 1, "H_mod_A", 1, true, {}, 0});
    expect(!host.lobby_game(invalid), "loaded map capacity refuses an extra owner row");
    expect(host.view().start_enabled, "refused map edit preserves readiness");
    host.start(selected);
    milestone = "Start announcement";
    wait([&] { return std::all_of(matches.begin(), matches.end(), [](const auto& match) {
        return match->lobby_state()->snapshot.phase == net::LobbyPhase::starting;
    }); });
    expect(host.lobby_kick(matches.back()->view().seat), "host can kick a seat during nine-seat Start");
    milestone = "Start cancellation";
    wait([&] { return host.lobby_state()->snapshot.phase == net::LobbyPhase::staging && !host.view().start_enabled; });
    expect(host.lobby_state()->snapshot.seats.size() == owners.size() - 1 && !host.view().start_enabled,
        "Start cancellation removes one controller and clears readiness");
    for (auto& match : matches) match->cancel();
}

void editable_staging_team_rules() {
    stage = "editable team and faction staging";
    const auto initial = setup();
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map; selected.match = initial.skirmish->match;
    selected.slots = std::vector<eawr::skirmish::LobbySlot>{{1, "Rebel", 0, true, {}, 0}, {2, "Empire", 1, true, {}, 1}};
    eawr::skirmish::SetupMap map; map.path = *selected.map; map.metadata.capacity = 2;
    map.metadata.new_markers = true; map.metadata.start_positions.emplace(2);
    map.teams = {{0, {"Rebel", "Empire"}, 1}, {1, {"Rebel", "Empire"}, 1}};
    auto structural = *selected.slots;
    structural[1].slot = structural[0].slot;
    expect(!eawr::skirmish::validate_setup_slots(structural, map.metadata, false, false), "editable staging still refuses duplicate rows");
    structural = *selected.slots; structural[1].team = 2;
    expect(!eawr::skirmish::validate_setup_slots(structural, map.metadata, false, false), "editable staging still requires an authored team");
    const viewer::LanLobbyBuilder builder = [initial](const auto&) { return eawr::core::Result<tactical::TacticalSetup>::success(initial); };
    auto host = make_match(), join = make_match();
    host->configure_lobby("Host", 0, {map}, builder); join->configure_lobby("Guest", 1, {map}, builder);
    host->host(0); host->pump_setup(selected);
    join->join("127.0.0.1:" + std::to_string(host->view().port));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    const auto wait = [&](const auto& predicate) {
        while (true) {
            host->pump_setup(selected); join->pump_setup({});
            if (predicate()) return;
            expect(std::chrono::steady_clock::now() < deadline, "editable staging deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    wait([&] { return host->lobby_state() && host->lobby_state()->snapshot.seats.size() == 2
        && join->lobby_state() && join->lobby_state()->snapshot == host->lobby_state()->snapshot
        && !join->lobby_state()->identity_pending; });
    auto edited = require(viewer::decode_lan_lobby_game(host->lobby_state()->snapshot));
    edited.slots->at(1).team = 0;
    expect(host->lobby_game(edited), "host can publish mixed-faction teammates");
    wait([&] { return join->lobby_state()->snapshot == host->lobby_state()->snapshot; });
    expect(!host->lobby_state()->notice && !join->lobby_state()->notice, "team edit emits no lobby error");
    {   // #4041: a row holding a human cannot be retyped (dropped to AI/Closed/Open); the refusal names its reason.
        const auto guest_slot = join->lobby_state()->snapshot.seats.back().selection.slot;
        auto retyped = edited;
        retyped.slots->erase(std::remove_if(retyped.slots->begin(), retyped.slots->end(), [&](const auto& row) { return row.slot == guest_slot; }), retyped.slots->end());
        const auto before = host->lobby_state()->snapshot;
        expect(!host->lobby_game(retyped), "host cannot retype a row holding a human");
        expect(host->lobby_state()->notice && host->lobby_state()->notice->reason == net::LobbyReason::invalid_choice
            && host->lobby_state()->notice->message.find("held by a human") != std::string::npos, "retype refusal names its reason");
        expect(host->lobby_state()->snapshot == before, "refused retype leaves the lobby unchanged");
        // The host's own row is held by a human too.
        auto host_retyped = edited;
        host_retyped.slots->erase(std::remove_if(host_retyped.slots->begin(), host_retyped.slots->end(), [&](const auto& row) { return row.slot == host->lobby_state()->snapshot.seats.front().selection.slot; }), host_retyped.slots->end());
        expect(!host->lobby_game(host_retyped), "host cannot retype its own row");
        expect(host->lobby_state()->snapshot == before, "refused host-row retype leaves the lobby unchanged");
        // Joiner side stays identical after the refusals, and a publish keeping every human row is still accepted.
        wait([&] { return join->lobby_state()->snapshot == before; });
        expect(host->lobby_game(edited), "a publish keeping every human row is accepted after a refusal");
        wait([&] { return join->lobby_state()->snapshot == host->lobby_state()->snapshot; });
    }
    for (const auto& match : {host, join}) {
        const auto state = match->lobby_state();
        expect(!state->ready_enabled && !state->start_enabled
            && state->ready_reason == "Players sharing a team must choose the same faction.", "invalid teams disable Ready and Start with a stable reason");
        expect(!match->lobby_ready(true) && !match->lobby_state()->local_ready, "illegal staging cannot Ready");
    }
    auto choice = join->lobby_state()->snapshot.seats.back().selection; choice.faction = "Rebel";
    expect(join->lobby_select(choice), "remote faction correction is allowed while setup remains one team");
    wait([&] { return host->lobby_state()->snapshot.seats.back().selection.faction == "Rebel"
        && join->lobby_state()->snapshot == host->lobby_state()->snapshot; });
    expect(host->lobby_state()->ready_reason == "Choose at least two different teams."
        && !host->lobby_state()->ready_enabled, "one-team intermediate edit stays visible with its own Ready reason");
    choice.team = 1;
    expect(join->lobby_select(choice), "remote team correction is accepted");
    wait([&] { return host->lobby_state()->snapshot.seats.back().selection.team == 1
        && join->lobby_state()->snapshot == host->lobby_state()->snapshot; });
    expect(host->lobby_state()->ready_reason.empty() && join->lobby_state()->ready_reason.empty()
        && host->lobby_state()->ready_enabled && join->lobby_state()->ready_enabled, "corrected setup clears reason and enables Ready");
    expect(host->lobby_ready(true) && join->lobby_ready(true), "both players Ready after correcting teams");
    wait([&] { return host->lobby_state()->start_enabled; });
    host->cancel(); join->cancel();
}

// #4041: three peers stage through the real lobby, the host picks the advanced options, and every peer's
// Start hands over the same match, which the production builder turns into the same applied match policy.
void advanced_options_three_peers() {
    stage = "advanced options on three peers";
    const std::vector<std::string> factions{"Parity_A", "Parity_B", "Parity_C"};
    const std::vector<tactical::PlayerId> owners{1, 2, 3};
    auto initial = human_setup(3);
    eawr::skirmish::FixtureOptions plain;
    plain.map = initial.skirmish->map; plain.match = initial.skirmish->match; plain.seed = initial.seed;
    // The synthetic map has no station markers and no unit tables, so starting units and the base stay off.
    plain.match->allow_heroes = plain.match->allow_superweapons = true;
    plain.match->pre_built_base = plain.match->free_starting_units = false;
    plain.slots.emplace();
    eawr::skirmish::SetupMap map; map.path = *plain.map;
    map.metadata.capacity = 3; map.metadata.new_markers = true; map.metadata.start_positions.emplace(4);
    for (unsigned row = 0; row < 3; ++row) {
        plain.slots->push_back({row + 1, factions[row], row, true, {}, row});
        map.teams.push_back({row, {factions[row]}, 1});
    }
    auto chosen = plain;
    chosen.match->allow_heroes = false; chosen.match->allow_superweapons = false;
    // The production chain: lobby options -> Fixture -> skirmish::build_start -> lan_recording_setup.
    const viewer::LanLobbyBuilder builder = [factions](const eawr::skirmish::FixtureOptions& options) {
        eawr::units::UnitTables tables;
        eawr::skirmish::Fixture fixture;
        fixture.map = *options.map; fixture.map_sha256 = std::string(64, 'a'); fixture.seed = options.seed.value_or(0);
        fixture.match = options.match; fixture.slots = *options.slots;
        eawr::skirmish::StartInputs inputs; inputs.map = fixture.map; inputs.map_sha256 = fixture.map_sha256;
        inputs.tables = &tables; inputs.lobby_colours.resize(9);
        for (unsigned team = 0; team < factions.size(); ++team) {
            eawr::skirmish::StartFaction faction; faction.name = factions[team]; faction.playable = true;
            inputs.factions.push_back(std::move(faction));
            eawr::skirmish::MapPlacement marker; marker.record = team;
            marker.type = "Team_0" + std::to_string(team) + "_Spawn_Point_Marker";
            marker.marker = true; marker.position = sim::math::Vec3{}; marker.orientation_degrees = sim::math::Vec3{};
            inputs.placements.push_back(std::move(marker));
        }
        auto start = eawr::skirmish::build_start(fixture, inputs);
        if (!start) return eawr::core::Result<tactical::TacticalSetup>::failure(start.error());
        return eawr::core::Result<tactical::TacticalSetup>::success(viewer::lan_recording_setup(fixture, start.value()));
    };
    const auto plain_policy = require(builder(plain)).match_policy;
    expect(plain_policy && plain_policy->allow_heroes && plain_policy->allow_superweapons, "the production builder keeps the enabled options");
    const auto chosen_setup = require(builder(chosen));
    expect(chosen_setup.match_policy && !chosen_setup.match_policy->allow_heroes && !chosen_setup.match_policy->allow_superweapons,
        "the production builder applies the disabled heroes and super weapons");
    expect(require(net::encode_skirmish_setup(chosen_setup)) != require(net::encode_skirmish_setup(require(builder(plain)))),
        "the setup bytes cover the advanced options");
    std::vector<std::shared_ptr<viewer::LanMatch>> matches;
    for (unsigned peer = 0; peer < 3; ++peer) {
        auto match = make_match(); match->configure_factions(factions); match->configure_seats(owners);
        match->configure_lobby("Adv_" + std::to_string(peer), peer, {map}, builder);
        matches.push_back(std::move(match));
    }
    auto& host = *matches.front(); host.host(0);
    const eawr::skirmish::FixtureOptions* current = &plain;
    host.pump_setup(*current);
    for (unsigned peer = 1; peer < 3; ++peer) matches[peer]->join("127.0.0.1:" + std::to_string(host.view().port));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const auto wait = [&](const auto& predicate) {
        while (true) {
            for (auto& match : matches) match->pump_setup(*current);
            if (predicate()) return;
            expect(std::chrono::steady_clock::now() < deadline, "advanced options staging deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    const auto synced = [&] {
        return std::all_of(matches.begin(), matches.end(), [&](const auto& match) {
            return match->lobby_state() && match->lobby_state()->snapshot == host.lobby_state()->snapshot && !match->lobby_state()->identity_pending;
        });
    };
    wait([&] { return host.lobby_state() && host.lobby_state()->snapshot.seats.size() == 3 && synced(); });
    const auto game_before = host.lobby_state()->snapshot.game;
    expect(!matches[1]->lobby_game(chosen) && !matches[2]->lobby_game(chosen), "a guest cannot publish advanced options");
    wait([&] { return synced(); });
    expect(host.lobby_state()->snapshot.game == game_before, "a refused guest edit leaves the room's game blob unchanged");
    current = &chosen;
    expect(host.lobby_game(chosen), "the host publishes the advanced options");
    wait([&] { return synced() && host.lobby_state()->snapshot.game != game_before; });
    for (const auto& match : matches) {
        const auto offered = match->offered_setup();
        expect(offered && offered->match && !offered->match->allow_heroes && !offered->match->allow_superweapons,
            "every peer sees the host's options before Ready");
    }
    for (auto& match : matches) expect(match->lobby_ready(true), "each peer readies the revision that carries the options");
    wait([&] { return host.view().start_enabled; });
    host.start(chosen);
    wait([&] { return std::all_of(matches.begin(), matches.end(), [](const auto& match) {
        return match->lobby_state()->snapshot.phase == net::LobbyPhase::starting;
    }); });
    std::vector<std::vector<std::uint8_t>> bytes;
    for (auto& match : matches) {
        const auto start = match->take_start();
        expect(start.has_value() && start->match && !start->match->allow_heroes && !start->match->allow_superweapons,
            "each peer's Start hands over the host's advanced options");
        const auto setup = require(builder(*start));
        expect(setup.match_policy && !setup.match_policy->allow_heroes && !setup.match_policy->allow_superweapons,
            "each peer's started match applies the options");
        bytes.push_back(require(net::encode_skirmish_setup(setup)));
    }
    expect(bytes[0] == bytes[1] && bytes[1] == bytes[2], "all three peers start from identical setup bytes");
    for (auto& match : matches) match->cancel();
}

void authoritative_staging(bool play, std::optional<net::LobbyReason> end_during_start) {
    stage = "authoritative viewer staging";
    auto initial = setup();
    for (auto& row : initial.skirmish->slots) row.fleet.clear();
    initial.queue_identities = true;
    initial.departure_policy.emplace(); initial.departure_policy->humans = {1, 2};
    initial.players[0].faction_id = eawr::skirmish::faction_id("Rebel");
    initial.players[1].faction_id = eawr::skirmish::faction_id("Empire");
    initial.players[2].faction_id = eawr::skirmish::faction_id("Underworld");
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map; selected.match = initial.skirmish->match; selected.seed = initial.seed;
    selected.victory_condition = tactical::VictoryCondition::enemy_starbase_destroyed;
    selected.slots = std::vector<eawr::skirmish::LobbySlot>{{1, "Rebel", 1, true, {}, 2}, {2, "Empire", 2, true, {}, 4}, {3, "Underworld", 3, false, {}, 1}};
    eawr::skirmish::SetupMap map; map.path = *selected.map; map.metadata.capacity = 3; map.metadata.new_markers = true; map.metadata.start_positions.emplace(4);
    map.teams = {{1, {"Rebel"}, 1}, {2, {"Empire"}, 1}, {3, {"Underworld"}, 1}};
    const viewer::LanLobbyBuilder builder = [initial](const auto& options) mutable {
        auto built = initial; built.seed = options.seed.value_or(initial.seed); built.skirmish->match = *options.match;
        built.match_policy = tactical::SkirmishMatchPolicy{options.match->allow_heroes, options.match->allow_superweapons,
            options.match->free_starting_units, options.match->pre_built_base};
        for (const auto& slot : *options.slots) {
            built.players[slot.slot - 1].faction_id = eawr::skirmish::faction_id(slot.faction);
            built.players[slot.slot - 1].team_id = slot.team;
            built.skirmish->slots[slot.slot - 1].colour_index = slot.colour_index;
            built.skirmish->slots[slot.slot - 1].human = slot.human;
        }
        return eawr::core::Result<tactical::TacticalSetup>::success(std::move(built));
    };
    auto host = make_match(), join = make_match();
    map.name = "Staging test map";
    const std::vector loaded_maps{map};
    host->configure_lobby("Alice", 4, loaded_maps, builder); join->configure_lobby("Bob", 5, loaded_maps, builder);
    expect(host->lobby_enabled() && join->lobby_enabled(), "authoritative chat capability exists before admission");
    expect(!join->lobby_chat("early line"), "room chat is refused until the first admitted snapshot");
    expect(!viewer::make_fake_lan_match("joined")->lobby_enabled(), "legacy presentation fixture retains source-specific chat");
    host->host(0); host->pump_setup(selected);
    const auto initial_room = require(viewer::decode_lan_lobby_game(host->lobby_state()->snapshot));
    expect(initial_room.slots->front().colour_index == 4U && initial_room.slots->at(1).colour_index != 4U,
        "host preferred colour is kept and a colliding vacant owner row gets a distinct colour");
    join->join("127.0.0.1:" + std::to_string(host->view().port));
    expect(!host->view().credential_provider_available && !host->view().reconnect_enabled,
        "authoritative staging disables the legacy default credential/recovery provider");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    const auto wait = [&](const auto& predicate) {
        while (!predicate()) {
            expect(std::chrono::steady_clock::now() < deadline, "staging bridge deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    const auto pump = [&] { host->pump_setup(selected); join->pump_setup({}); };
    wait([&] { pump(); const auto state = host->lobby_state(); return state && state->snapshot.seats.size() == 2 && state->snapshot.seats[1].selection.name == "Bob" && join->lobby_state() && join->lobby_state()->snapshot == state->snapshot; });
    expect(!host->view().start_enabled, "open links never imply readiness");
    expect(join->offered_setup() && join->offered_setup()->map == selected.map, "joiner sees live map before Start");
    const auto offered = join->offered_setup();
    expect(!join->lobby_state()->local_ready && offered->match == selected.match,
        "joiner receives the host map and all selected options before Accept");
    const auto loaded_map = std::find_if(loaded_maps.begin(), loaded_maps.end(),
        [&](const auto& candidate) { return candidate.path == *offered->map; });
    expect(loaded_map != loaded_maps.end() && loaded_map->name == map.name,
        "received map id resolves to the loaded display name before Accept");
    const auto distinct_revision = host->lobby_state()->snapshot.revision;
    auto duplicate_colour = host->lobby_state()->snapshot.seats.front().selection;
    duplicate_colour.colour = host->lobby_state()->snapshot.seats.back().selection.colour;
    expect(!host->lobby_select(duplicate_colour) && host->lobby_state()->snapshot.revision == distinct_revision,
        "duplicate owner colours are refused before publishing a revision");
    expect(host->lobby_ready(true) && join->lobby_ready(true), "each human explicitly readies the current revision");
    wait([&] { pump(); return host->view().start_enabled; });
    const auto old_revision = host->lobby_state()->snapshot.revision;
    auto choice = join->lobby_state()->snapshot.seats[1].selection; choice.name = "Bobby";
    expect(join->lobby_select(choice), "own name request");
    wait([&] { pump(); return host->lobby_state()->snapshot.revision > old_revision; });
    expect(host->view().start_enabled, "name-only edit preserves all readiness");
    expect(join->lobby_chat("room message"), "admitted room chat");
    wait([&] { pump(); return host->lobby_state()->chat.size() == 1 && join->lobby_state()->chat.size() == 1; });
    expect(host->lobby_state()->chat.front().name == "Bobby", "host stamps the authoritative sender label");
    auto edited = selected; edited.match->allow_heroes = !edited.match->allow_heroes;
    // #4041: every Advanced Options field the lobby dialog edits travels in the one host-owned game blob.
    edited.match->free_starting_units = !edited.match->free_starting_units;
    edited.match->allow_superweapons = !edited.match->allow_superweapons;
    auto all_enabled = require(builder(selected)); all_enabled.match_policy.reset(); // all-enabled uses the legacy encoding
    const auto selected_bytes = require(net::encode_skirmish_setup(all_enabled));
    auto forged = edited; forged.match->allow_heroes = selected.match->allow_heroes;
    const auto host_game_before = host->lobby_state()->snapshot.game;
    expect(!join->lobby_game(forged), "a guest cannot publish advanced options");
    pump();
    expect(host->lobby_state()->snapshot.game == host_game_before && join->lobby_state()->snapshot.game == host_game_before,
        "a refused guest options edit leaves every peer's game blob unchanged");
    expect(host->lobby_game(edited), "host changes loaded application options");
    wait([&] { pump(); return !join->lobby_state()->local_ready; });
    expect(!host->view().start_enabled, "consequential options clear both ready icons and Start");
    expect(join->offered_setup()->match == edited.match,
        "options-only host edit reaches the joiner without a map or roster edit");
    expect(join->lobby_state()->snapshot.game == host->lobby_state()->snapshot.game,
        "advanced options round-trip host to guest byte for byte");
    expect(require(net::encode_skirmish_setup(require(builder(*join->offered_setup())))) != selected_bytes,
        "the setup bytes every peer agrees on cover the advanced options");
    {
        const auto policy = require(builder(*join->offered_setup())).match_policy;
        expect(policy && policy->allow_heroes == edited.match->allow_heroes && policy->allow_superweapons == edited.match->allow_superweapons
            && policy->free_starting_units == edited.match->free_starting_units, "the joiner's built match applies the host's options");
    }
    if (!play) {
        expect(!join->lobby_kick(0), "joiner cannot kick host");
        expect(host->lobby_kick(1), "host kicks admitted joiner");
        wait([&] { pump(); return join->view().phase == viewer::LanPhase::ended; });
        expect(join->lobby_state()->notice && join->lobby_state()->notice->reason == net::LobbyReason::kicked, "typed kick reaches the removed player");
        expect(host->lobby_state()->snapshot.seats.size() == 1 && !host->view().start_enabled, "kick clears readiness and removes exactly one seat");
        expect(host->lobby_game(edited), "after the kick the freed row no longer blocks a publish");
        host->pump_setup(edited); host->pump_setup(edited);
        auto fresh = make_match(); fresh->configure_lobby("Carol", 4, {map}, builder);
        fresh->join("127.0.0.1:" + std::to_string(host->view().port));
        wait([&] { host->pump_setup(edited); fresh->pump_setup({});
            return host->lobby_state()->snapshot.seats.size() == 2 && host->lobby_state()->snapshot.seats.back().selection.name == "Carol"; });
        expect(fresh->view().seat == 1 && host->lobby_state()->snapshot.seats.back().seat.value > 2, "a fresh edge refills the same transport index with a new stable seat identity");
        expect(!host->view().start_enabled, "fresh admission clears every ready flag");
        host->cancel(); join->cancel(); fresh->cancel(); return;
    }
    // Human pacing opens a fresh bounded request window.
    std::this_thread::sleep_for(std::chrono::milliseconds(2001)); pump();
    expect(host->lobby_ready(true) && join->lobby_ready(true), "reaccept edited revision");
    wait([&] { pump(); return host->view().start_enabled; });
    auto canonical = require(builder(*host->offered_setup()));
    const auto staging_bytes = require(net::encode_skirmish_setup(canonical));
    expect(staging_bytes == require(net::encode_skirmish_setup(require(builder(*join->offered_setup())))), "staging builders agree on exact live offer bytes");
    net::SetupRecoveryPolicy unsupported; unsupported.resync = true;
    host->set_recovery_policy(unsupported);
    host->start(edited);
    expect(!host->take_start() && host->lobby_state()->notice && host->lobby_state()->notice->reason == net::LobbyReason::recovery_unavailable
        && host->lobby_state()->snapshot.phase == net::LobbyPhase::staging, "unsupported recovery is a distinct refusal before Start mutates the room");
    host->set_recovery_policy({});
    host->start(edited);
    if (!end_during_start) expect(host->take_start().has_value(), "host hands frozen choices to the live loader");
    // Pump the outer stream without core/tick-zero publication until the live bind.
    wait([&] { host->pump_setup(edited); join->pump_setup({}); return join->lobby_state()->snapshot.phase == net::LobbyPhase::starting; });
    expect(join->take_start().has_value(), "joiner hands frozen choices to the same live loader");
    if (end_during_start) {
        if (*end_during_start == net::LobbyReason::kicked) expect(host->lobby_kick(1), "host kick before its own handover cancels Start");
        else host->lobby_leave();
        host->pump_setup(edited);
        expect(!host->take_start(), "aborted Start cannot hand stale choices or a deleted setup stream to the live loader");
    }
    sim::InlineExecutor executor;
    std::array<tactical::TacticalSession, 2> worlds{require(tactical::TacticalSession::create(canonical, content())), require(tactical::TacticalSession::create(canonical, content()))};
    std::array<eawr::platform::LiveStepContext, 2> contexts;
    std::array<eawr::platform::LiveStepDriver, 2> drivers{host->driver(canonical), join->driver(canonical)};
    for (std::size_t peer = 0; peer < 2; ++peer) {
        auto& context = contexts[peer]; context.advance_allowed = false;
        context.completed_tick = [&, peer] { return worlds[peer].completed_tick(); };
        context.state_hash = [&, peer] { return worlds[peer].state_sha256(); };
        context.canonical_state = [&, peer] { return worlds[peer].canonical_state_bytes(); };
        context.install = [&, peer](std::span<const tactical::PlayerCommand> commands) { for (const auto& command : commands) require(worlds[peer].submit(command)); return eawr::core::Result<void>::success(); };
        context.step = [&, peer] { require(worlds[peer].step(executor)); return eawr::core::Result<void>::success(); };
        context.snapshot = [&, peer] { return worlds[peer].snapshot(); };
    }
    const auto clock = [] { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); };
    if (end_during_start) {
        wait([&] { host->pump_setup(edited); auto driven = drivers[1](contexts[1], {}, clock());
            return !driven && join->view().phase == viewer::LanPhase::ended; });
        expect(join->view().message == (*end_during_start == net::LobbyReason::kicked ? "TEXT_INTERNET_PLAYER_KICKED" : "TEXT_HOST_LEFT_GAME"), "Start-phase terminal notice uses its distinct localized key");
        expect(join->view().diagnostic_code == net::lobby_diagnostic(*end_during_start, "").code, "Start-phase notice retains its exact lobby diagnostic");
        join->lobby_leave(); join->cancel(); host->cancel(); return;
    }
    for (unsigned round = 0; round < 20; ++round) for (std::size_t peer = 0; peer < 2; ++peer) require(drivers[peer](contexts[peer], {}, clock()));
    expect(worlds[0].completed_tick() == 0 && worlds[1].completed_tick() == 0, "live load holds tick zero until both peers may advance");
    for (auto& context : contexts) context.advance_allowed = true;
    wait([&] { for (std::size_t peer = 0; peer < 2; ++peer) if (worlds[peer].completed_tick() < 10) require(drivers[peer](contexts[peer], {}, clock())); return worlds[0].completed_tick() == 10 && worlds[1].completed_tick() == 10; });
    expect(worlds[0].state_sha256() == worlds[1].state_sha256(), "LobbySession commits through SetupSession into identical battle state");
    host->cancel(); join->cancel();
}
}
