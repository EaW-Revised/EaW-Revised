#include "setup_test_support.hpp"
#include "online_match.hpp"
#include "online_service_settings.hpp"
#include "eawr/net/gns_transport.hpp"
#include "eawr/platform/sim_workers.hpp"
#include <array>
#include <atomic>
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
        expect(!viewer::OnlineMatchOptions{}.transport_observer && !system_options.transport_observer,
            "default and settings-derived production options leave the test transport observer unset");
        expect(!system_options.identity.empty() && system_options.identity.size() <= net::gns_identity_max_bytes,
            "settings factory identity fits the signalling and GNS transport limit");
        expect(system_options.service.certificate_sha256.empty() && system_options.service.ca_file.empty()
            && system_options.service.certificate_name == "service.example", "system trust cannot fall back to inactive pin or CA fields");
        const std::string role = argv[1]; const bool host = role.starts_with("host") || role == "cancel" || role == "retry";
        const bool leave_case = role.find("-leave") != std::string::npos;
        const bool replacement = role.find("-new-") != std::string::npos;
        const bool nine = role.find("-nine") != std::string::npos || role.find("-leave9") != std::string::npos;
        const bool multi = nine || leave_case;
        const auto participant = multi && !host ? static_cast<unsigned>(std::stoul(role.substr(role.rfind('-') + 1))) : 0U;
        const std::uint32_t seat_count = nine ? 9U : leave_case ? 3U : 2U;
        const std::string player_name = host ? "Host" : multi ? "Guest" + std::to_string(participant) : "Guest";
        viewer::OnlineMatchOptions options; options.service.address = "127.0.0.1";
        options.service.port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        options.service.ca_file = argv[3]; options.service.certificate_name = "localhost";
        options.identity = host ? "bridge-host" : "bridge-guest"; options.short_invitation = true;
        if (multi && !host) options.identity += (replacement ? "new" : "") + std::to_string(participant);
        const bool directory_room = role.find("directory-") != std::string::npos;
        const bool private_room = role.ends_with("private");
        std::shared_ptr<net::OnlineRoomSource> rooms;
        // Declared before the match: closed after it, so no source worker runs into process exit.
        struct RoomCloser { std::shared_ptr<net::OnlineRoomSource>& source; ~RoomCloser() { if (source) source->close(); } } closer{rooms};
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
                viewer::bind_online_room_source(options, rooms);
            }
        }
        if (role == "guest-mismatch") options.build_identity += "-different";
        // Windows can take seconds to report a refused localhost connect.
        // Let that refusal finish instead of racing the introduction timeout.
        if (role == "retry") options.service.timeout = 5s;
        if (role == "host-no-code") options.require_short_code = true;
        if (host && !directory_room) {
            // The host publishes a plain directory room and its match opens the signalling scope on it (no bridge invitation).
            rooms = viewer::host_standalone_room(options, "Bridge", seat_count);
            expect(static_cast<bool>(rooms), "standalone room source configures");
        }
        if (role == "host-silent") {
            // A real room host whose guest pair opens but that deliberately never sends a staging snapshot.
            const auto deadline = std::chrono::steady_clock::now() + 30s;
            std::optional<net::OnlineHostedRoom> hosted;
            while (!(hosted = rooms->hosted_room())) {
                expect(std::chrono::steady_clock::now() < deadline, "silent host publication deadline");
                std::this_thread::sleep_for(2ms);
            }
            auto room = require(net::NetworkGnsRoom::open(options.service, hosted->publication, hosted->principal, options.identity, true));
            std::ofstream(argv[4]) << room->alias();
            // The service pushes the roster right after the admission reply; the transport needs the room size and this seat.
            while (room->roster().capacity == 0) {
                expect(std::chrono::steady_clock::now() < deadline, "silent host roster deadline");
                std::this_thread::sleep_for(2ms);
            }
            net::GnsOptions transport_options; transport_options.build_identity = options.build_identity;
            transport_options.max_frame_size = static_cast<std::uint32_t>(net::max_lobby_frame_bytes);
            transport_options.peers = room->roster().capacity;
            auto transport = require(net::GnsTransport::host_room(room, transport_options));
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
        net::GnsTransport* observed_transport{};
        if (nine) options.transport_observer = [&](net::GnsTransport& transport) { observed_transport = &transport; };
        auto match = viewer::make_online_match(options);
        struct LogProof {
            std::atomic<unsigned> installs{}, clears{}, transport_lines{};
        };
        const auto log_proof = std::make_shared<LogProof>();
        match->set_diagnostic_logger([log_proof](std::string_view line) {
            if (line.starts_with("EAWR net event=gns sink install gen=") && line.ends_with(" match=online")) ++log_proof->installs;
            else if (line.starts_with("EAWR net event=gns sink clear gen=") && line.ends_with(" match=online")) ++log_proof->clears;
            else if (line.starts_with("EAWR net event=gns ")) ++log_proof->transport_lines;
        });
        std::ofstream host_status;
        if (nine && host) {
            host_status.open(std::string(argv[4]) + ".host-status");
            host_status << std::unitbuf;
            std::cout << std::unitbuf;
            std::cerr << std::unitbuf;
        }
        std::string previous_host_status;
        const auto report_host = [&](const char* event, bool force = false) {
            if (!host_status.is_open()) return;
            const auto view = match->view();
            std::string details = "phase=" + std::to_string(static_cast<unsigned>(view.phase))
                + " connected=" + std::to_string(view.online_connected) + "/" + std::to_string(view.online_capacity)
                + " diagnostic=" + view.diagnostic_code + " message=" + view.message;
            if (const auto state = match->lobby_state()) {
                details += " room-phase=" + std::to_string(static_cast<unsigned>(state->snapshot.phase))
                    + " revision=" + std::to_string(state->snapshot.revision)
                    + " frozen=" + std::to_string(state->snapshot.frozen_revision)
                    + " seats=" + std::to_string(state->snapshot.seats.size())
                    + " preparing=" + std::to_string(state->preparing)
                    + " start=" + std::to_string(state->start_enabled);
                for (const auto& seat : state->snapshot.seats)
                    details += " [peer=" + std::to_string(seat.peer) + " ready=" + std::to_string(seat.ready) + "]";
                if (state->notice) details += " notice=" + state->notice->message;
            }
            if (force || details != previous_host_status) host_status << event << " " << details << '\n';
            previous_host_status = std::move(details);
        };
        match->configure_factions({"Rebel", "Empire", "Underworld"});
        // Guests deliberately keep the default two-seat placeholder: the attached roster must supply nine, including code joins.
        if (nine && host) match->configure_seats({1, 2, 3, 4, 5, 6, 7, 8, 9});
        else if (leave_case && host) match->configure_seats({1, 2, 3});
        if (nine && participant == 8) match->configure_seats({1, 1}); // A previous invalid host selection cannot poison a new join.
        if (nine && host) {
            auto named_refusal_options = options;
            named_refusal_options.hosted_room = [] { return std::optional<net::OnlineHostedRoom>{}; };
            const auto refusal_text = net::mesh_newcomer_refusal_text("build version", true);
            named_refusal_options.hosting_failure = [refusal_text] {
                eawr::core::Diagnostic error; error.code = "EAWR-NET-1762"; error.message = refusal_text;
                return std::optional{error};
            };
            auto refused_match = viewer::make_online_match(std::move(named_refusal_options));
            refused_match->configure_factions({"Rebel", "Empire", "Underworld"});
            refused_match->host(0);
            const auto refusal_deadline = std::chrono::steady_clock::now() + 10s;
            while (refused_match->view().phase != viewer::LanPhase::ended) {
                refused_match->pump_setup({});
                expect(std::chrono::steady_clock::now() < refusal_deadline, "named typed refusal completes promptly");
                std::this_thread::sleep_for(2ms);
            }
            expect(refused_match->view().reason == viewer::LanReason::refused
                && refused_match->view().message.starts_with(refusal_text),
                "a player's name cannot turn a typed pair refusal into a build/version mismatch");
            refused_match->cancel();
        }
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
        if (nine) {
            initial = human_setup(9); initial.queue_identities = true; initial.departure_policy.emplace();
            for (auto& player : initial.players) {
                player.team_id = (player.player_id - 1) / 3 + 1;
                initial.departure_policy->humans.push_back(player.player_id);
            }
        }
        else if (leave_case) initial.departure_policy->humans = {1, 2, 3};
        for (auto& row : initial.skirmish->slots) row.fleet.clear();
        eawr::skirmish::FixtureOptions selected;
        selected.map = initial.skirmish->map; selected.match = initial.skirmish->match; selected.seed = initial.seed;
        selected.victory_condition = tactical::VictoryCondition::enemy_starbase_destroyed;
        selected.slots.emplace();
        const std::array factions{"Rebel", "Empire", "Underworld"};
        for (std::size_t i = 0; i < initial.players.size(); ++i) {
            const auto team = nine ? i / 3 : i;
            initial.players[i].faction_id = eawr::skirmish::faction_id(factions[team]);
            selected.slots->push_back({static_cast<unsigned>(i + 1), factions[team], static_cast<unsigned>(team + 1),
                initial.skirmish->slots[i].human, {}, nine ? static_cast<unsigned>(i) : std::array{4U, 5U, 1U}[i]});
        }
        if (multi) selected.slots->back().human = true;
        eawr::skirmish::SetupMap map; map.path = *selected.map; map.metadata.capacity = nine ? 9 : 3;
        map.metadata.new_markers = true; map.metadata.start_positions.emplace(4);
        const auto markers = nine ? 3U : 1U;
        map.teams = {{1, {"Rebel"}, markers}, {2, {"Empire"}, markers}, {3, {"Underworld"}, markers}};
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
        match->configure_lobby(player_name, host ? 4 : 5, {map}, builder);
        const auto deadline = std::chrono::steady_clock::now() + (multi ? 90s : 45s);
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
            std::string code;
            std::ifstream(nine && directory_room ? std::string(argv[4]) + ".room" : std::string(argv[4])) >> code;
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
                    rooms->begin_join({eawr::presentation::ui::LobbySourceKind::online, generation}, code, password, player_name);
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
                expect(admitted.admission && !admitted.admission->member_capability.empty(), "directory admission hands the guest its room member capability");
                match->join_admitted(std::move(*admitted.admission));
            } else {
                expect(code.size() == 4, "server-generated short code is shared"); match->join(code);
            }
        }
        if (role == "host-bad-setup") selected.slots.reset();
        expect(log_proof->installs >= 2 && log_proof->clears >= 1,
            "host/join clears the configured sink then installs a fresh online sink");
        bool written{}, saw_snapshot{}, ready_requested{}, attachment_written{}, row_move_requested{}, row_move_configured{};
        unsigned refill_stage{};
        std::uint64_t ready_revision{};
        const auto terminal_proof = [&](const std::string& expected) {
            expect(match->view().diagnostic_code == expected && match->view().message.find(expected) != std::string::npos,
                "both online view fields expose the typed refusal: " + match->view().message);
            std::ofstream(std::string(argv[4]) + "." + role) << expected;
            // The invalid host fails before any guest can attach: it proves its refusal alone and does not wait for a partner.
            while (role != "host-bad-setup" && role != "host-no-code" && !std::filesystem::exists(std::string(argv[4]) + ".done")) { match->pump_setup(selected); pause(); }
            match->cancel(); viewer::shutdown_lan_drains();
        };
        while (true) {
            match->pump_setup(selected);
            report_host("setup pump");
            const auto view = match->view();
            if (view.phase == viewer::LanPhase::ended) {
                if (role == "refused") {
                    expect(view.diagnostic_code == "EAWR-NET-1523", "The closed room's alias refuses a later guest");
                    match->cancel(); viewer::shutdown_lan_drains(); return 0;
                }
                if (role == "guest-mismatch") { terminal_proof("EAWR-NET-1503"); return 0; }
                if (role == "host-bad-setup") { terminal_proof("EAWR-NET-1535"); return 0; }
                if (role == "host-no-code") { terminal_proof("EAWR-NET-1787"); return 0; }
                if (role == "guest-silent") { terminal_proof("EAWR-NET-1536"); return 0; }
                expect(false, "online admission: " + view.message);
            }
            // The host lobby exists once the room's signalling scope is open: only then does a directory admission carry a capability.
            if (host && !written && match->lobby_state() && (directory_room || !view.invitation_code.empty())) {
                expect(view.invitation_expires > std::chrono::steady_clock::now(), "accepted invitation TTL shown");
                if (nine) {
                    const auto published_room = rooms->hosted_room();
                    expect(published_room.has_value(), "nine-seat publication remains owned");
                    // The last guest exercises room-list admission in the same nine-seat match; the other seven use the code.
                    std::ofstream(std::string(argv[4]) + ".room") << published_room->publication.room;
                }
                if (directory_room) {
                    if (private_room) expect(view.invitation_code.empty(), "private game capability never enters the display view");
                    if (private_room || role.ends_with("no-alias")) expect(view.invitation_code.empty(), "no room alias appears for a private or alias-less room");
                    const auto hosted = rooms->hosted_room();
                    if (!hosted) { pause(); continue; }
                    if (!std::filesystem::exists(std::string(argv[4]) + ".private-audit")) {
                        std::ofstream audit(std::string(argv[4]) + ".private-audit");
                        audit << hosted->principal << '\n' << hosted->publication.ownership << '\n' << "private-room" << '\n' << "wrong-room-password";
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
            if (role == "host-mismatch" && written && std::filesystem::exists(std::string(argv[4]) + ".guest-mismatch")) {
                // A guest of another build is refused at its own pair: the room keeps staging for the next player.
                const auto staged = match->lobby_state();
                expect(staged && staged->snapshot.seats.size() == 1, "a guest of another build is not admitted to the room");
                std::ofstream(std::string(argv[4]) + "." + role) << "staging";
                while (!std::filesystem::exists(std::string(argv[4]) + ".done")) { match->pump_setup(selected); pause(); }
                match->cancel(); viewer::shutdown_lan_drains(); return 0;
            }
            if (role.ends_with("-refill") && (written || !host)) {
                // A guest leaves the room and the same player (same roster identity, same slot) comes back: the host must admit
                // it again instead of answering "removed from the room".
                const auto staged = match->lobby_state();
                const auto seats = staged ? staged->snapshot.seats.size() : std::size_t{};
                const auto finished = std::filesystem::exists(std::string(argv[4]) + ".done");
                if (host) {
                    if (refill_stage == 0 && seats == 2) refill_stage = 1;
                    else if (refill_stage == 1 && seats == 1) refill_stage = 2;
                    else if (refill_stage == 2 && seats == 2) { std::ofstream(std::string(argv[4]) + "." + role) << "refilled"; refill_stage = 3; }
                    else if (refill_stage == 3 && finished) { match->cancel(); viewer::shutdown_lan_drains(); return 0; }
                } else if (refill_stage == 0 && seats == 2) {
                    match->lobby_leave();
                    for (unsigned i = 0; i < 100; ++i) { match->pump_setup(selected); std::this_thread::sleep_for(2ms); }
                    match->cancel();
                    std::this_thread::sleep_for(500ms);
                    match = viewer::make_online_match(options);
                    match->configure_factions({"Rebel", "Empire", "Underworld"});
                    match->configure_lobby("Guest", 5, {map}, builder);
                    std::string code; std::ifstream(argv[4]) >> code;
                    match->join(code);
                    refill_stage = 1;
                } else if (refill_stage == 1 && seats == 2) {
                    std::ofstream(std::string(argv[4]) + "." + role) << "rejoined"; refill_stage = 2;
                } else if (refill_stage == 2 && finished) { match->cancel(); viewer::shutdown_lan_drains(); return 0; }
                pause();
                continue;
            }
            if (leave_case) {
                const auto staged = match->lobby_state();
                const auto seats = staged ? staged->snapshot.seats.size() : std::size_t{};
                const auto marker = std::string(argv[4]);
                if (host && !nine && !row_move_configured && seats == 2) {
                    const auto moved = std::find_if(staged->snapshot.seats.begin(), staged->snapshot.seats.end(), [](const auto& seat) {
                        return seat.peer == 1 && seat.player == 3 && seat.selection.slot == 3;
                    });
                    if (moved != staged->snapshot.seats.end()) {
                        selected.match->credits = require(sim::math::add(selected.match->credits, units(1)));
                        expect(match->lobby_game(selected), "host changes game options after a guest moves to the future peer's row");
                        auto planned = match->lobby_state()->snapshot; planned.seats.clear();
                        const auto configured = require(viewer::decode_lan_lobby_game(planned));
                        expect(configured.match->credits == selected.match->credits && configured.slots->size() == 3
                            && std::all_of(configured.slots->begin(), configured.slots->end(), [](const auto& row) { return row.human; }),
                            "game updates retain the original human-row pool, including the vacated row");
                        std::ofstream(marker + ".row-moved") << "configured"; row_move_configured = true;
                    }
                }
                if (!host && staged && !staged->identity_pending && !attachment_written) {
                    const auto local = std::find_if(staged->snapshot.seats.begin(), staged->snapshot.seats.end(), [&](const auto& seat) {
                        return seat.peer == view.seat && seat.selection.name == player_name;
                    });
                    if (local != staged->snapshot.seats.end()) {
                        if (!nine && !replacement && participant == 1) {
                            if (!row_move_requested) {
                                auto choice = local->selection; choice.slot = 3;
                                expect(match->lobby_select(choice), "first guest requests the future peer's open human row");
                                row_move_requested = true;
                            }
                            if (local->player != 3 || local->selection.slot != 3 || !std::filesystem::exists(marker + ".row-moved")) {
                                pause(); continue;
                            }
                        }
                        std::ofstream(marker + ".joined-" + role) << "attached";
                        attachment_written = true;
                    }
                }
                bool attached = true;
                if (host) for (unsigned peer = 1; peer < seat_count; ++peer)
                    attached = attached && std::filesystem::exists(marker + ".joined-guest-leave" + std::to_string(seat_count) + "-" + std::to_string(peer));
                if (host && refill_stage == 0 && seats == seat_count && view.online_connected == seat_count && attached) {
                    if (!nine) {
                        expect(row_move_configured, "the first guest moved before the last guest joined");
                        for (const auto& seat : staged->snapshot.seats)
                            expect(seat.player == (seat.peer == 0 ? 1U : seat.peer == 1 ? 3U : 2U)
                                && seat.selection.slot == seat.player,
                                "the late guest fills the lowest vacant human row without moving its predecessor");
                    }
                    std::ofstream(marker + ".leave-go") << "leave";
                    // Hold the lobby pump until both guest processes have drained
                    // and exited, so the next pump observes both closed links.
                    while (!std::filesystem::exists(marker + ".resume")) pause();
                    refill_stage = 1;
                } else if (host && refill_stage == 1 && seats == seat_count - 2) {
                    expect(staged->snapshot.phase == net::LobbyPhase::staging && view.diagnostic_code.empty(),
                        "both departures reopen seats without ending the online host");
                    std::ofstream(marker + ".vacant") << "vacant";
                    refill_stage = 2;
                } else if (host && refill_stage == 2 && seats == seat_count && view.online_connected == seat_count) {
                    std::ofstream(marker + ".refilled") << "refilled";
                    refill_stage = 3;
                }
                if (!host && !replacement && participant >= seat_count - 2
                    && std::filesystem::exists(marker + ".leave-go")) {
                    match->lobby_leave(); match->cancel(); viewer::shutdown_lan_drains();
                    std::ofstream(marker + ".departed-" + role) << "left";
                    return 0;
                }
                if (!std::filesystem::exists(marker + ".refilled")) { pause(); continue; }
            }
            const auto state = match->lobby_state();
            if (state) {
                expect(!state->snapshot.room.empty(), "authoritative online staging snapshot received"); saw_snapshot = true;
                if (nine && host && state->snapshot.phase == net::LobbyPhase::staging && !state->start_enabled)
                    expect(!state->start_reason.empty(), "an online Start blocker always has a player-facing reason");
                expect(!state->notice || role == "host-mismatch", "online staging readiness notice: " + (state->notice ? state->notice->message : std::string{}));
                if (nine && !host && !attachment_written && !state->identity_pending) {
                    // Let the launcher admit the next member only after this reservation has attached and its name is acknowledged.
                    const auto local = std::find_if(state->snapshot.seats.begin(), state->snapshot.seats.end(), [&](const auto& seat) {
                        return seat.peer == view.seat && seat.selection.name == player_name;
                    });
                    if (local != state->snapshot.seats.end()) {
                        std::ofstream(std::string(argv[4]) + ".joined-" + role) << "attached";
                        attachment_written = true;
                    }
                }
            }
            if (state && ready_requested && !state->preparing && !state->local_ready
                && state->snapshot.revision != ready_revision) ready_requested = false;
            if (state && !ready_requested && state->snapshot.phase == net::LobbyPhase::staging
                && state->snapshot.seats.size() == seat_count
                && state->snapshot.seats.front().selection.name == "Host"
                && (multi ? std::all_of(state->snapshot.seats.begin() + 1, state->snapshot.seats.end(),
                    [](const auto& seat) { return seat.selection.name.starts_with("Guest"); })
                    : state->snapshot.seats.back().selection.name == "Guest")
                && (!nine || view.online_connected == seat_count)
                && !state->identity_pending && !state->local_ready && !state->preparing) {
                if (nine) {
                    expect(observed_transport && !observed_transport->room_locked(), "the native room is unlocked before this seat readies");
                    expect(view.online_capacity == 9 && view.online_connections.size() == 9, "all nine online connection rows are published");
                    expect(std::all_of(view.online_connections.begin(), view.online_connections.end(), [](const auto& line) {
                        return line.find("Connected") != std::string::npos;
                    }), "all local pairs are visibly connected before Ready");
                }
                ready_requested = match->lobby_ready(true);
                ready_revision = state->snapshot.revision;
            }
            if (host && view.start_enabled) {
                report_host("Start begin", true);
                match->start(selected);
                report_host("Start return", true);
            }
            if (auto chosen = match->take_start()) { selected = std::move(*chosen); break; }
            pause();
        }
        expect(saw_snapshot, "online setup starts only after an authoritative staging snapshot");
        expect(match->view().invitation_consumed && match->view().invitation_code.empty(), "pairing consumes displayed invitation");
        auto canonical = require(builder(selected));
        auto world = require(tactical::TacticalSession::create(canonical, content()));
        const auto workers = nine ? std::array{1U, 2U, 4U, 8U}[participant % 4] : 1U;
        eawr::platform::ThreadWorkerAdapter executor(workers);
        eawr::platform::LiveStepContext context; context.advance_allowed = true;
        context.completed_tick = [&] { return world.completed_tick(); };
        context.state_hash = [&] { return world.state_sha256(); };
        context.canonical_state = [&] { return world.canonical_state_bytes(); };
        context.install = [&](std::span<const tactical::PlayerCommand> commands) { for (const auto& command : commands) require(world.submit(command)); return eawr::core::Result<void>::success(); };
        context.step = [&] { require(world.step(executor)); return eawr::core::Result<void>::success(); };
        context.snapshot = [&] { return world.snapshot(); };
        auto driver = match->driver(canonical);
        unsigned driver_pumps{};
        bool faulted{}, saw_repair_hold{};
        std::uint64_t original_generation{};
        while (world.completed_tick() < 10) {
            // The final directory guest has a canonical room ID for refresh.
            // Fault only on this same I/O/driver owner, after committed battle ticks.
            if (nine && !leave_case && participant == 8 && !faulted && world.completed_tick() >= 3) {
                expect(observed_transport != nullptr, "the owner observes its native transport");
                expect(observed_transport->room_locked(), "the native guest sees Lock during committed battle");
                original_generation = observed_transport->edge_identity(0);
                expect(original_generation != 0, "the interrupted pair had an authenticated generation");
                require(observed_transport->interrupt_peer(0)); faulted = true;
            }
            const auto clock = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            if (driver_pumps < 8) report_host("driver pump begin", true);
            auto stepped = driver(context, {}, clock);
            report_host("driver pump return", driver_pumps++ < 8);
            if (!stepped) {
                std::string notice;
                if (const auto state = match->lobby_state(); state && state->notice)
                    notice = " lobby notice=" + state->notice->message;
                expect(false, "driver role=" + role + " " + eawr::core::format_diagnostic(stepped.error()) + notice);
            }
            if (nine && world.completed_tick() > 0)
                expect(observed_transport && observed_transport->room_locked(),
                    "the validated lobby commitment locks every native seat before its first battle tick");
            if (faulted && match->view().phase == viewer::LanPhase::reconnecting) {
                if (!saw_repair_hold) expect(match->view().message == "Connection to Host: Refreshing room credentials. Play is paused.",
                    "the faulted guest's first hold names Host and the exact credential-refresh phase");
                saw_repair_hold = true;
                expect(!viewer::lan_gameplay_enabled(match->view()) && viewer::lan_recovery_audio_suppressed(match->view()),
                    "mesh repair closes gameplay and audio admission");

            }
            pause();
        }
        if (nine && !leave_case && participant == 8) {
            expect(faulted && saw_repair_hold, "the viewer observed a real in-battle repair hold");
            expect(observed_transport->edge_identity(0) > original_generation
                && observed_transport->pair(0).phase == net::GnsPairPhase::open,
                "native repair replaced the authenticated pair before battle resumed");
            expect(match->view().phase == viewer::LanPhase::running, "the room confirmation released the viewer hold");
            std::ofstream(std::string(argv[4]) + ".nine-repair") << "restored";
        }
        std::cout << "tick10 " << world.state_sha256() << '\n';
        // Keep both streams alive until the harness acknowledges both tick-zero and battle proofs.
        std::ofstream(std::string(argv[4]) + "." + role) << world.state_sha256();
        while (!std::filesystem::exists(std::string(argv[4]) + ".done")) { match->pump_setup(selected); pause(); }
        driver = {};
        if (nine && !leave_case && participant == 8)
            expect(log_proof->transport_lines > 0, "real GNS repair diagnostics reach the configured online logger");
        const auto clears = log_proof->clears.load();
        match->cancel();
        expect(log_proof->clears == clears + 1, "cancel records the active online sink generation being cleared");
        viewer::shutdown_lan_drains(); return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; viewer::shutdown_lan_drains(); return 1; }
}
