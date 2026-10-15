#include "setup_test_support.hpp"
#include "online_match.hpp"
#include "online_service_settings.hpp"
#include "eawr/net/gns_transport.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

using namespace setup_test;
namespace viewer = eawr::presentation::godot_backend;
using namespace std::chrono_literals;
int main(int argc, char** argv) {
    try {
        if (!net::GnsTransport::available() || !net::NetworkGnsIntroduction::available()) return 77;
        expect(argc == 5, "role, relay port, trust file and invitation file required");
        eawr::presentation::ui::NetworkSettings system_settings;
        system_settings.online_host = "service.example"; system_settings.online_trust = "system";
        system_settings.online_pin = std::string(64, 'a'); system_settings.online_ca_file = "inactive-file";
        const auto system_options = require(viewer::online_match_options(system_settings));
        expect(!system_options.identity.empty() && system_options.identity.size() <= net::gns_identity_max_bytes,
            "settings factory identity fits the signalling and GNS transport limit");
        expect(system_options.service.certificate_sha256.empty() && system_options.service.ca_file.empty()
            && system_options.service.certificate_name == "service.example", "system trust cannot fall back to inactive pin or CA fields");
        const std::string role = argv[1]; const bool host = role.starts_with("host") || role == "cancel" || role == "retry";
        viewer::OnlineMatchOptions options; options.service.address = "127.0.0.1";
        options.service.port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        options.service.ca_file = argv[3]; options.service.certificate_name = "localhost";
        options.identity = host ? "bridge-host" : "bridge-guest"; options.short_invitation = true;
        const bool directory_room = role.find("directory-") != std::string::npos;
        const bool private_room = role.ends_with("private");
        std::shared_ptr<net::OnlineRoomSource> rooms;
        if (directory_room) {
            eawr::presentation::ui::NetworkSettings settings;
            settings.online_host = "localhost"; settings.online_port = options.service.port;
            settings.online_trust = "ca-file"; settings.online_ca_file = argv[3];
            options = require(viewer::online_match_options(settings));
            rooms = viewer::make_live_online_source(options);
            if (host) {
                expect(rooms->set_host_password(private_room ? "private-room" : ""), "host password configures live source");
                eawr::presentation::ui::LobbyPresence presence;
                presence.place = eawr::presentation::ui::LobbyPlace::hosting; presence.name = "Host";
                presence.game = private_room ? "Directory private" : "Directory public";
                presence.map = "map-from-data"; presence.game_type = "space"; presence.max_players = 2;
                presence.passworded = private_room; rooms->announce(presence);
            }
        }
        if (role == "guest-mismatch") options.build_identity += "-different";
        // Windows can take seconds to report a refused localhost connect.
        // Let that refusal finish instead of racing the introduction timeout.
        if (role == "retry") options.service.timeout = 5s;
        if (role == "host-silent") {
            // A real admitted GNS peer deliberately never sends a staging snapshot.
            auto published = require(net::NetworkGnsIntroduction(options.service).publish(options.identity, 15min));
            std::ofstream(argv[4]) << published.room_alias;
            const auto deadline = std::chrono::steady_clock::now() + 30s;
            while (published.endpoint->rendezvous().remote_identity.empty()) {
                require(published.endpoint->status());
                expect(std::chrono::steady_clock::now() < deadline, "silent host pairing deadline");
                std::this_thread::sleep_for(2ms);
            }
            net::GnsOptions transport_options; transport_options.build_identity = options.build_identity;
            transport_options.max_frame_size = static_cast<std::uint32_t>(net::max_lobby_frame_bytes);
            auto transport = require(net::GnsTransport::host_introduced(published.endpoint, transport_options));
            bool admitted{};
            while (!std::filesystem::exists(std::string(argv[4]) + ".done")) {
                expect(!transport->poll(), "silent peer receives no staging application frames");
                if (!admitted && transport->link(1) == net::LinkState::open) {
                    std::ofstream(std::string(argv[4]) + "." + role) << "silent"; admitted = true;
                }
                expect(std::chrono::steady_clock::now() < deadline, "silent peer proof deadline");
                std::this_thread::sleep_for(2ms);
            }
            expect(admitted, "silent peer completed the real GNS build handshake"); transport->cancel(); return 0;
        }
        if (role == "guest-bad-setup") {
            // The invalid host closes admission; check its typed service cleanup
            // separately from the live-but-silent first-snapshot timeout below.
            std::string code; std::ifstream(argv[4]) >> code;
            auto joined = net::NetworkGnsIntroduction(options.service).join(code, options.identity);
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            auto status = joined ? joined.value()->status() : eawr::core::Result<void>::failure(joined.error());
            while (status) {
                expect(std::chrono::steady_clock::now() < deadline, "invalid host closes its service session");
                std::this_thread::sleep_for(2ms); status = joined.value()->status();
            }
            expect(status.error().code == "EAWR-NET-1528", "Invalid staging host releases the paired service session");
            std::ofstream(std::string(argv[4]) + "." + role) << status.error().code;
            while (!std::filesystem::exists(std::string(argv[4]) + ".done")) {
                expect(std::chrono::steady_clock::now() < deadline, "invalid host proof acknowledgement");
                std::this_thread::sleep_for(2ms);
            }
            return 0;
        }
        auto match = viewer::make_online_match(options);
        match->configure_factions({"Rebel", "Empire", "Underworld"});
        if (role == "retry") {
            match->host(0);
            const auto began = std::chrono::steady_clock::now();
            const auto deadline = began + 10s;
            while (match->view().phase != viewer::LanPhase::ended) {
                expect(std::chrono::steady_clock::now() < deadline, "closed-port publication fails promptly");
                match->pump_setup({}); std::this_thread::sleep_for(2ms);
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
            expect(match->view().diagnostic_code == "EAWR-NET-1519", "closed-port failure is typed: "
                + match->view().diagnostic_code + " after " + std::to_string(elapsed) + "ms");
            do {
                std::this_thread::sleep_for(2ms); match->host(0);
                expect(std::chrono::steady_clock::now() < deadline, "failed publication retry must not wait for invitation TTL");
            } while (match->view().phase != viewer::LanPhase::connecting);
            match->cancel(); viewer::shutdown_lan_drains(); return 0;
        }
        auto initial = setup(); initial.queue_identities = true;
        initial.departure_policy.emplace(); initial.departure_policy->humans = {1, 2};
        for (auto& row : initial.skirmish->slots) row.fleet.clear();
        for (std::size_t i = 0; i < 3; ++i)
            initial.players[i].faction_id = eawr::skirmish::faction_id(std::array{"Rebel", "Empire", "Underworld"}[i]);
        eawr::skirmish::FixtureOptions selected;
        selected.map = initial.skirmish->map; selected.match = initial.skirmish->match; selected.seed = initial.seed;
        selected.victory_condition = tactical::VictoryCondition::enemy_starbase_destroyed;
        selected.slots = std::vector<eawr::skirmish::LobbySlot>{{1, "Rebel", 1, true, {}, 4}, {2, "Empire", 2, true, {}, 5}, {3, "Underworld", 3, false, {}, 1}};
        eawr::skirmish::SetupMap map; map.path = *selected.map; map.metadata.capacity = 3;
        map.metadata.new_markers = true; map.metadata.start_positions.emplace(4);
        map.teams = {{1, {"Rebel"}, 1}, {2, {"Empire"}, 1}, {3, {"Underworld"}, 1}};
        const viewer::LanLobbyBuilder builder = [initial](const auto& choices) {
            auto built = initial; built.seed = choices.seed.value_or(initial.seed); built.skirmish->match = *choices.match;
            const tactical::SkirmishMatchPolicy policy{choices.match->allow_heroes, choices.match->allow_superweapons,
                choices.match->free_starting_units, choices.match->pre_built_base};
            if (!policy.allow_heroes || !policy.allow_superweapons || !policy.free_starting_units || !policy.pre_built_base)
                built.match_policy = policy;
            else built.match_policy.reset(); // All-enabled choices use the canonical default encoding.
            for (const auto& slot : *choices.slots) {
                built.players[slot.slot - 1].faction_id = eawr::skirmish::faction_id(slot.faction);
                built.players[slot.slot - 1].team_id = slot.team;
                built.skirmish->slots[slot.slot - 1].colour_index = slot.colour_index;
                built.skirmish->slots[slot.slot - 1].human = slot.human;
            }
            return eawr::core::Result<tactical::TacticalSetup>::success(std::move(built));
        };
        match->configure_lobby(host ? "Host" : "Guest", host ? 4 : 5, {map}, builder);
        const auto deadline = std::chrono::steady_clock::now() + 25s;
        const auto pause = [&] {
            std::string details;
            if (const auto state = match->lobby_state()) {
                details = " revision=" + std::to_string(state->snapshot.revision) + " preparing=" + std::to_string(state->preparing);
                for (const auto& seat : state->snapshot.seats) details += " [" + seat.selection.name + " ready=" + std::to_string(seat.ready) + "]";
                if (state->notice) details += " notice=" + state->notice->message;
            }
            expect(std::chrono::steady_clock::now() < deadline, "online bridge deadline role=" + role + " phase=" + std::to_string(static_cast<unsigned>(match->view().phase)) + " seats=" + std::to_string(match->view().connected_seats) + " diagnostic=" + match->view().diagnostic_code + details + ": " + match->view().message);
            std::this_thread::sleep_for(2ms);
        };
        if (host) { match->set_public_invitation(!private_room); match->host(0); }
        else {
            std::string code; std::ifstream(argv[4]) >> code;
            if (directory_room) {
                expect(code.size() == 32, "directory room id is shared, never its private invitation");
                bool listed = false;
                while (!listed) {
                    static_cast<void>(rooms->pump());
                    for (const auto& row : rooms->games()) if (row.address == code) {
                        expect(row.version_matches && row.passworded == private_room, "live room tags/password are displayed"); listed = true;
                    }
                    if (!listed) pause();
                }
                const auto admit = [&](std::uint64_t generation, const std::string& password) {
                    rooms->begin_join({eawr::presentation::ui::LobbySourceKind::online, generation}, code, password, "Guest");
                    for (;;) {
                        auto replies = rooms->take_join_results();
                        if (!replies.empty()) return std::move(replies.front());
                        pause();
                    }
                };
                if (private_room) {
                    const auto wrong = admit(1, "wrong-room-password");
                    expect(wrong.refusal && wrong.refusal->code == "EAWR-NET-1546" && !wrong.admission, "wrong password releases no game binding");
                }
                auto admitted = admit(2, private_room ? "private-room" : "");
                expect(admitted.admission && admitted.admission->bridge_invitation.size() == 32, "directory admission hands the bridge its one-use invitation");
                match->join(std::move(admitted.admission->bridge_invitation));
            } else {
                expect(code.size() == 4, "server-generated short code is shared"); match->join(code);
            }
        }
        if (role == "host-bad-setup") selected.slots.reset();
        bool written{}, saw_snapshot{}, ready_requested{};
        std::uint64_t ready_revision{};
        const auto terminal_proof = [&](const std::string& expected) {
            expect(match->view().diagnostic_code == expected && match->view().message.find(expected) != std::string::npos,
                "both online view fields expose the typed refusal: " + match->view().message);
            std::ofstream(std::string(argv[4]) + "." + role) << expected;
            while (!std::filesystem::exists(std::string(argv[4]) + ".done")) { match->pump_setup(selected); pause(); }
            match->cancel(); viewer::shutdown_lan_drains();
        };
        while (true) {
            match->pump_setup(selected);
            const auto view = match->view();
            if (view.phase == viewer::LanPhase::ended) {
                if (role == "refused") {
                    expect(view.diagnostic_code == "EAWR-NET-1523", "Cancelled bridge invitation refuses a later guest");
                    match->cancel(); viewer::shutdown_lan_drains(); return 0;
                }
                if (role.ends_with("mismatch")) { terminal_proof("EAWR-NET-1503"); return 0; }
                if (role == "host-bad-setup") { terminal_proof("EAWR-NET-1535"); return 0; }
                if (role == "guest-silent") { terminal_proof("EAWR-NET-1536"); return 0; }
                expect(false, "online admission: " + view.message);
            }
            if (host && !written && (directory_room ? !match->bridge_invitation().empty() : !view.invitation_code.empty())) {
                expect(view.invitation_expires > std::chrono::steady_clock::now(), "accepted invitation TTL shown");
                if (directory_room) {
                    if (private_room) expect(view.invitation_code.empty(), "private game capability never enters the display view");
                    rooms->set_bridge_invitation(match->bridge_invitation(), match->bridge_ownership());
                    if (private_room || role.ends_with("no-alias")) expect(view.invitation_code.empty(), "no full capability appears without a public alias");
                    if (!std::filesystem::exists(std::string(argv[4]) + ".private-audit")) {
                        std::ofstream audit(std::string(argv[4]) + ".private-audit");
                        audit << match->bridge_invitation() << '\n' << match->bridge_ownership() << '\n' << "private-room" << '\n' << "wrong-room-password";
                    }
                    const auto listed = rooms->games();
                    const auto found = std::find_if(listed.begin(), listed.end(), [&](const auto& row) {
                        return row.name == (private_room ? "Directory private" : "Directory public");
                    });
                    if (found == listed.end()) { pause(); continue; }
                    std::ofstream(argv[4]) << found->address;
                } else std::ofstream(argv[4]) << view.invitation_code;
                written = true;
                if (role == "cancel") {
                    const auto before = std::chrono::steady_clock::now(); match->cancel();
                    expect(std::chrono::steady_clock::now() - before < 100ms, "cancel never waits for publication teardown");
                    for (unsigned i = 0; i < 50; ++i) { match->pump_setup(selected); std::this_thread::sleep_for(2ms); }
                    expect(match->view().phase == viewer::LanPhase::idle && match->view().invitation_code.empty(), "stale publication cannot re-enter cancelled generation");
                    viewer::shutdown_lan_drains(); return 0;
                }
            }
            const auto state = match->lobby_state();
            if (state) {
                expect(!state->snapshot.room.empty(), "authoritative online staging snapshot received"); saw_snapshot = true;
                expect(!state->notice, "online staging readiness notice: " + (state->notice ? state->notice->message : std::string{}));
            }
            if (state && ready_requested && !state->preparing && !state->local_ready
                && state->snapshot.revision != ready_revision) ready_requested = false;
            if (state && !ready_requested && state->snapshot.phase == net::LobbyPhase::staging
                && state->snapshot.seats.size() == 2
                && state->snapshot.seats.front().selection.name == "Host"
                && state->snapshot.seats.back().selection.name == "Guest"
                && !state->identity_pending && !state->local_ready && !state->preparing) {
                ready_requested = match->lobby_ready(true);
                ready_revision = state->snapshot.revision;
            }
            if (host && view.start_enabled) match->start(selected);
            if (auto chosen = match->take_start()) { selected = std::move(*chosen); break; }
            pause();
        }
        expect(saw_snapshot, "online setup starts only after an authoritative staging snapshot");
        expect(match->view().invitation_consumed && match->view().invitation_code.empty(), "pairing consumes displayed invitation");
        auto canonical = require(builder(selected));
        auto world = require(tactical::TacticalSession::create(canonical, content())); sim::InlineExecutor executor;
        eawr::platform::LiveStepContext context; context.advance_allowed = true;
        context.completed_tick = [&] { return world.completed_tick(); };
        context.state_hash = [&] { return world.state_sha256(); };
        context.canonical_state = [&] { return world.canonical_state_bytes(); };
        context.install = [&](std::span<const tactical::PlayerCommand> commands) { for (const auto& command : commands) require(world.submit(command)); return eawr::core::Result<void>::success(); };
        context.step = [&] { require(world.step(executor)); return eawr::core::Result<void>::success(); };
        context.snapshot = [&] { return world.snapshot(); };
        auto driver = match->driver(canonical);
        while (world.completed_tick() < 10) {
            const auto clock = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            require(driver(context, {}, clock)); pause();
        }
        std::cout << "tick10 " << world.state_sha256() << '\n';
        // Keep both streams alive until the harness acknowledges both tick-zero and battle proofs.
        std::ofstream(std::string(argv[4]) + "." + role) << world.state_sha256();
        while (!std::filesystem::exists(std::string(argv[4]) + ".done")) { match->pump_setup(selected); pause(); }
        driver = {}; match->cancel(); viewer::shutdown_lan_drains(); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; viewer::shutdown_lan_drains(); return 1; }
}
