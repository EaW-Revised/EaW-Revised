#include "setup_test_support.hpp"
#include "lan_match.hpp"
#include "skirmish_prepare.hpp"
#include "../../src/net/platform_socket.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/lan_credentials.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "fake_transport.hpp"
#include <chrono>
#include <barrier>
#include <iostream>
#include <thread>
#include <algorithm>
#include <map>
#include <set>

namespace viewer = eawr::presentation::godot_backend;
using namespace setup_test;

namespace {
std::string stage;
std::map<int, std::string> recovery_worker_hashes;
std::shared_ptr<viewer::LanMatch> make_match(std::filesystem::path root = {}, std::uint64_t delay = 3,
    bool progress = false, std::filesystem::path trace = {}) {
    auto match = viewer::make_lan_match(std::move(root), delay, progress, std::move(trace));
    match->configure_factions({"Rebel", "Empire", "Underworld", "Mod_Fleet_C"});
    return match;
}
void player_row_parser() {
    expect(require(viewer::skirmish_player_ids("1,2,3,4,5,6,7,8")).size() == 8, "eight local faction rows parse");
    for (const auto text : {"1,2,3,4,5,6,7,8,9", "1,2,", "1,1", "0,2", "1", "1,x"})
        expect(!viewer::skirmish_player_ids(text), "invalid local rows refuse at parse time");
}

void recovery_presentation_contract() {
    stage = "recovery presentation table";
    struct Row { const char* state; viewer::LanRecovery recovery; const char* message; bool gameplay; };
    const Row rows[]{
        {"waiting", viewer::LanRecovery::waiting, "Waiting for Opponent...", true},
        {"held", viewer::LanRecovery::held, "Connection lost. Reconnecting...", false},
        {"catching_up", viewer::LanRecovery::catching_up, "Rejoining game...", false},
        {"proving", viewer::LanRecovery::proving, "Checking synchronized state...", false},
        {"recovery_barrier", viewer::LanRecovery::proving, "Checking synchronized state...", false},
        {"rejoin_failed", viewer::LanRecovery::rejoin_failed, "Could not rejoin this game.", false},
        {"desync_paused", viewer::LanRecovery::desync, "Players' game states differ. Play is paused.", false},
        {"resyncing", viewer::LanRecovery::resyncing, "Restoring synchronized game state...", false},
        {"recovered", viewer::LanRecovery::recovered, "Game synchronized.", true},
        {"recovery_failed", viewer::LanRecovery::failed, "Synchronization could not be restored.", false},
        {"recovery_disabled", viewer::LanRecovery::failed, "Synchronization could not be restored.", false},
    };
    for (const auto& row : rows) {
        auto match = viewer::make_fake_lan_match(row.state);
        const auto view = match->view();
        expect(view.recovery == row.recovery, "every recovery table row has a distinct view state");
        expect(viewer::lan_message(view).starts_with(row.message), "recovery table uses curated player text");
        eawr::presentation::ui::CommandScheduler scheduler(1);
        std::string feedback;
        viewer::LanCommandSink sink(scheduler, match, [&](const auto& error) { feedback = error.message; });
        eawr::presentation::ui::TacticalIntent move;
        move.verb = eawr::presentation::ui::TacticalVerb::move; move.units = {1};
        expect(static_cast<bool>(sink.issue(move)) == row.gameplay, "all gameplay routes share recovery admission");
        eawr::presentation::ui::TacticalIntent buy; buy.verb = eawr::presentation::ui::TacticalVerb::buy;
        buy.units = {1}; buy.type = 42;
        eawr::presentation::ui::TacticalIntent beacon; beacon.verb = eawr::presentation::ui::TacticalVerb::beacon;
        expect(static_cast<bool>(sink.issue(buy)) == row.gameplay && static_cast<bool>(sink.issue(beacon)) == row.gameplay,
            "economy and beacon use the same recovery admission as movement");
        expect(row.gameplay ? feedback.empty() : !feedback.empty(), "recovery refusal provides visible feedback");
        expect(scheduler.pending() == (row.gameplay ? 3U : 0U), "recovering input queues nothing");
        expect(scheduler.next_sequence() == (row.gameplay ? 3U : 0U), "refused recovery input consumes no sequence");
        if (row.gameplay) {
            expect(!match->request_abort(), "playing Leave uses agreed command policy");
            eawr::presentation::ui::TacticalIntent leave; leave.verb = eawr::presentation::ui::TacticalVerb::intentional_quit;
            expect(static_cast<bool>(sink.issue(leave)), "playing Leave submits ordinary quit");
        } else {
            bool stopped{};
            expect(viewer::try_abort_lan(*match, [&] { stopped = true; }) && stopped,
                "Leave in every nonplaying recovery state stops the local owner before cleanup");
            expect(!match->view().connected, "Leave ends the local recovery attempt");
        }
    }
    auto held = viewer::make_fake_lan_match("held")->view();
    const auto held_text = viewer::lan_message(held);
    expect(held_text.find("remaining: 12 s") != std::string::npos && held_text.find("Grace 17 s") != std::string::npos
        && held_text.find("timeout 23 s") != std::string::npos && held_text.find("cap 91 s") != std::string::npos,
        "presentation uses negotiated values rather than default timer literals");
    expect(viewer::lan_message(viewer::make_fake_lan_match("catching_up")->view()).find("tick: 87") != std::string::npos,
        "catch-up exposes verified completed-tick progress");
    viewer::LanView wait; wait.phase = viewer::LanPhase::running; wait.recovery = viewer::LanRecovery::waiting;
    wait.peer_name = "Guest\xE2\x80\xAE\xE2\x81\xA6\xD8\x9C\n";
    const auto safe = viewer::lan_message(wait);
    expect(safe.find("\xE2\x80\xAE") == std::string::npos && safe.find("\xE2\x81\xA6") == std::string::npos
        && safe.find("\xD8\x9C") == std::string::npos && safe.starts_with("Waiting for Guest "),
        "peer names cannot inject directional controls or lines");
    auto failed = viewer::make_fake_lan_match("rejoin_failed")->view();
    failed.diagnostic_code = std::string(net::reconnect_errors::auth);
    expect(viewer::lan_message(failed).find("authenticated") != std::string::npos, "authentication failure has a safe reason");
    failed.diagnostic_code = std::string(net::reconnect_errors::timeout);
    expect(viewer::lan_message(failed).find("no longer available") != std::string::npos, "expired seat has a safe reason");
    failed.diagnostic_code = "EAWR-NET-1025";
    expect(viewer::lan_message(failed).find("Local reconnect credentials are unavailable") != std::string::npos,
        "local provider failure is distinct from returning stream authentication");
    failed.diagnostic_code = "EAWR-NET-1026";
    expect(viewer::lan_message(failed).find("credential limit") != std::string::npos, "local limit has its exact reason");
    viewer::LanView terminal; terminal.phase = viewer::LanPhase::ended; terminal.reason = viewer::LanReason::timeout;
    terminal.recovery = viewer::LanRecovery::waiting; terminal.message = "Waiting for Opponent...";
    expect(viewer::lan_message(terminal) == "The other player did not respond in time.", "terminal timeout wins over stale waiting");
    terminal.message = "Reconnect is unavailable for this match. Play can continue.";
    terminal.reason = viewer::LanReason::peer_left;
    expect(viewer::lan_message(terminal) == "Opponent left.", "terminal departure wins over stale unavailable copy");
    terminal.message.clear(); terminal.recovery = viewer::LanRecovery::recovered; terminal.reason = viewer::LanReason::peer_left; terminal.desync_tick = 120;
    expect(viewer::lan_message(terminal) == "Opponent left.", "departure wins over prior recovered incident");
    for (const auto recovery : {viewer::LanRecovery::held, viewer::LanRecovery::catching_up, viewer::LanRecovery::proving,
        viewer::LanRecovery::resyncing, viewer::LanRecovery::desync}) {
        terminal.recovery = recovery;
        expect(!viewer::lan_recovery_audio_suppressed(terminal), "LAN outcome and summary audio bypass recovery suppression at match end");
        terminal.phase = viewer::LanPhase::running;
        expect(viewer::lan_recovery_audio_suppressed(terminal), "live replay recovery suppresses audio cues");
        terminal.phase = viewer::LanPhase::ended;
    }
    auto recovered = viewer::make_fake_lan_match("recovered");
    viewer::LanRecoveredNotice clock;
    expect(!clock.advance(recovered->view(), 1000, false) && !clock.advance(recovered->view(), 1299, false),
        "recovered clock begins at the published tick and holds before ten seconds");
    expect(!clock.advance(recovered->view(), 1500, true), "paused presented frames do not count");
    expect(clock.advance(recovered->view(), 1501, false), "only unpaused game frames complete the ten seconds");
    auto held_clock = viewer::make_fake_lan_match("held");
    expect(!clock.advance(held_clock->view(), 5000, false), "a new recovery incident resets the notice clock");
    expect(!clock.advance(recovered->view(), 8000, false), "a recovery publication jump does not age the notice");
    auto publication = recovered->view(); publication.active_adapter_publications = 1;
    expect(!clock.advance(publication, 20000, false), "a fresh publication restarts the notice even if the UI missed the intermediate recovery phase");
    auto unavailable = viewer::make_fake_lan_match("reconnect_unavailable");
    expect(viewer::lan_message(unavailable->view()).empty() && viewer::lan_gameplay_enabled(unavailable->view())
        && viewer::lan_incident_message(unavailable->view()) == "Reconnect is unavailable for this match. Play can continue.",
        "healthy disabled reconnect stays on the non-modal incident line");
    auto unsaved = viewer::make_fake_lan_match("recovered_unsaved");
    unsaved->acknowledge_recovery();
    expect(unsaved->view().diagnostic_available && !unsaved->view().diagnostic_saved
        && viewer::lan_incident_message(unsaved->view()).find("Retry Save") != std::string::npos,
        "acknowledged recovery retains the unsaved retry action and accurate status");
    expect(unsaved->save_diagnostic_details() && unsaved->view().diagnostic_saved,
        "local save retry updates explicit saved status after acknowledgment");
    expect(viewer::lan_message(unsaved->view()).empty(), "confirming or retrying from the incident line never opens a permanent overlay");
    expect(viewer::lan_incident_message(unsaved->view()).find("saved locally") != std::string::npos,
        "saved state never treats an error message as a saved path");
    for (const auto* code : {"EAWR-NET-1020", "EAWR-NET-1021", "EAWR-NET-1022", "EAWR-NET-1023", "EAWR-NET-1024", "EAWR-NET-1027"}) {
        failed.diagnostic_code = code;
        expect(viewer::lan_message(failed).starts_with("Could not rejoin this game.")
            && viewer::lan_message(failed).find("reserved seat") == std::string::npos,
            "credential policy or delivery failures use generic return copy");
    }
    expect(recovered->save_diagnostic_details() && recovered->view().recovery == viewer::LanRecovery::recovered,
        "saving local details preserves recovered command authority");
    expect(viewer::lan_message(recovered->view()).find("already saved automatically") != std::string::npos, "local save has player feedback");
    recovered->acknowledge_recovery();
    expect(recovered->view().recovery == viewer::LanRecovery::none && recovered->view().diagnostic_available
        && recovered->view().desync_tick == 120 && viewer::lan_gameplay_enabled(recovered->view()),
        "acknowledging recovery keeps incident details and gameplay authority");
    expect(viewer::lan_message(recovered->view()).empty(), "acknowledgment clears the overlay without discarding diagnostics");
}

void canonical_join_ping_key() {
    stage = "join endpoint matches lobby ping key";
    for (const auto& [typed, expected] : std::vector<std::pair<std::string, std::string>>{
        {"127.0.0.1", "127.0.0.1:47624"}, {"[::1]:47624", "::1:47624"}}) {
        auto join = make_match(); join->join(typed);
        expect(join->view().remote_endpoint == expected, "join stores the resolved host and explicit default port");
        viewer::LanView measured = join->view(); measured.connected = true;
        net::LinkStats observation; observation.smoothed_rtt = std::chrono::milliseconds(8); observation.sample_count = 1;
        measured.link_stats = observation;
        std::vector<eawr::presentation::ui::LobbyGame> games(1); games.front().address = expected;
        viewer::apply_lan_lobby_ping(games, measured);
        expect(games.front().ping_ms == 8, "typed endpoint updates the corresponding lobby ping row");
        join->cancel();
    }
}
void transport_failure_copy() {
    stage = "legacy transport failure copy";
    for (const bool version : {false, true}) {
        net::TcpOptions options; options.build_identity = eawr::core::build_identity();
        if (version) ++options.protocol_version; else options.build_identity += "-different";
        auto host = require(net::TcpTransport::host("127.0.0.1", 0, options));
        auto join = make_match(); join->join("127.0.0.1:" + std::to_string(host->port()));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (join->view().phase != viewer::LanPhase::ended) {
            expect(std::chrono::steady_clock::now() < deadline, "failure copy handshake deadline");
            (void)host->poll(); join->pump_setup({}); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(viewer::lan_message(join->view()) == (version ? "The other player uses a different network version." :
            "The other player uses a different game build."), "existing mismatch copy remains curated");
        auto viewing_host = make_match(); viewing_host->host(0);
        auto client = require(net::TcpTransport::connect("127.0.0.1", viewing_host->view().port, options));
        while (viewing_host->view().reason == viewer::LanReason::none) {
            expect(std::chrono::steady_clock::now() < deadline, "host failure copy handshake deadline");
            (void)client->poll(); viewing_host->pump_setup({}); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto curated = version ? "The other player uses a different network version." : "The other player uses a different game build.";
        const auto retry = viewing_host->view().phase == viewer::LanPhase::connecting;
        expect(viewer::lan_message(viewing_host->view()) == std::string(curated) + (retry ? " Waiting for a player to join..." : ""),
            "host mismatch and retry retain complete curated sentences");
    }
}

void connect_failure_copy() {
    stage = "connection refusal and deadline copy";
    for (const bool timeout : {false, true}) {
        const auto listener = net::socket::listen("127.0.0.1", 0);
        expect(listener.handle != net::socket::invalid, "failure copy listener");
        struct Guard { net::socket::Handle handle; ~Guard() { net::socket::close(handle); } } listener_guard{listener.handle};
        auto join = make_match(); join->join("127.0.0.1:" + std::to_string(listener.port));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        net::socket::Handle raw = net::socket::invalid;
        while (raw == net::socket::invalid) {
            expect(std::chrono::steady_clock::now() < deadline, "failure copy accept deadline");
            join->pump_setup({}); raw = net::socket::accept(listener.handle);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Guard raw_guard{raw};
        // Deliver the typed diagnostic through the real TCP refusal envelope, then EOF.
        // Socket contracts separately prove that local connect failures produce this code.
        std::vector<std::uint8_t> body{static_cast<std::uint8_t>(net::TcpMessage::refuse)};
        const auto append = [](auto& bytes, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        };
        const auto string = [&](const std::string& value) {
            append(body, static_cast<std::uint32_t>(value.size())); body.insert(body.end(), value.begin(), value.end());
        };
        string("EAWR-NET-0209");
        string(timeout ? "Could not connect to any resolved address before the connection deadline." :
            "Could not connect to any resolved address. Check the host and port.");
        std::vector<std::uint8_t> wire; append(wire, static_cast<std::uint32_t>(body.size())); wire.insert(wire.end(), body.begin(), body.end());
        std::size_t written = 0;
        while (written < wire.size()) {
            expect(std::chrono::steady_clock::now() < deadline, "failure copy write deadline");
            const auto io = net::socket::write(raw, std::span<const std::uint8_t>(wire).subspan(written));
            expect(io.status == net::socket::Status::ready || io.status == net::socket::Status::pending, "failure copy wire send");
            written += io.count;
        }
        expect(net::socket::shutdown_write(raw), "failure copy EOF");
        while (join->view().phase != viewer::LanPhase::ended) {
            expect(std::chrono::steady_clock::now() < deadline, "failure copy delivery deadline");
            join->pump_setup({}); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(viewer::lan_message(join->view()) == (timeout ? "The other player did not respond in time." :
            "The other player refused the connection."), "connect refusal and timeout retain curated copy");
    }
}
void threaded_loading_pair(int abort_side = -1, bool before_accept = false, bool admission_race = false) {
    stage = "threaded loading and review handover";
    auto initial = setup();
    initial.players[0].faction_id = eawr::skirmish::faction_id("Rebel");
    initial.players[1].faction_id = eawr::skirmish::faction_id("Empire");
    initial.players[2].faction_id = eawr::skirmish::faction_id("Underworld");
    auto host = make_match({}, 3, true), join = make_match({}, 3, true);
    host->host(0); join->join("127.0.0.1:" + std::to_string(host->view().port));
    expect(host->view().mapping.phase == eawr::net::MappingPhase::disabled, "router mapping is opt-in for ordinary hosts");
    auto mapped_view = host->view(); mapped_view.mapping = {eawr::net::MappingPhase::mapped, "203.0.113.7:40000", {}, "Share 203.0.113.7:40000"};
    expect(viewer::lan_message(mapped_view).find("203.0.113.7:40000") != std::string::npos, "host status exposes router endpoint for sharing");
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map;
    selected.slots = std::vector<eawr::skirmish::LobbySlot>{
        {1, "Rebel", 1, true, {"fleet-a"}}, {2, "Empire", 2, true, {"fleet-b"}}, {3, "Underworld", 3, false, {"fleet-ai"}}};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const auto wait = [&](const auto& condition) {
        while (!condition()) {
            expect(std::chrono::steady_clock::now() < deadline, "threaded LAN deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    wait([&] { host->pump_setup(selected); join->pump_setup(selected); return host->view().connected && join->view().connected; });
    host->start(selected); expect(host->take_start().has_value(), "host scene handover");
    eawr::platform::LiveSession::Options options;
    options.workers = 2; options.initially_paused = true;
    options.pacing = eawr::platform::LiveSession::Pacing::real_time;
    options.target_rate = 1000; options.step_driver = host->driver(initial);
    auto host_world = require(eawr::platform::LiveSession::start(initial, content(), options));
    wait([&] { join->pump_setup(selected); return join->view().accept_enabled; });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    expect(host_world->completed_tick() == 0, "host sim thread pumps while loading and reviewing");
    if (before_accept) {
        expect(viewer::lan_can_abort(host->view()), "host has an exit during human review");
        if (abort_side == 0) {
            expect(viewer::try_abort_lan(*host, host_world.get()), "host abort closes without a tick");
            wait([&] { join->pump_setup(selected); return join->view().phase == viewer::LanPhase::ended; });
            expect(viewer::try_abort_lan(*join, nullptr), "reviewer can leave after host abort");
        } else {
            join->cancel();
            wait([&] { return host->view().phase == viewer::LanPhase::ended; });
            expect(viewer::try_abort_lan(*host, host_world.get()), "host exits after reviewer leaves");
        }
        expect(host_world->completed_tick() == 0, "review abort never runs a tick");
        return;
    }
    join->accept(); expect(join->take_start().has_value(), "accepted join scene handover");
    options.step_driver = join->driver(initial);
    auto join_world = require(eawr::platform::LiveSession::start(initial, content(), options));
    wait([&] { return host_world->failure() || join_world->failure() ||
        (host->view().phase == viewer::LanPhase::barrier && join->view().phase == viewer::LanPhase::barrier); });
    if (abort_side >= 0) {
        auto leaving = abort_side == 0 ? host : join;
        auto remaining = abort_side == 0 ? join : host;
        auto* leaving_world = abort_side == 0 ? host_world.get() : join_world.get();
        auto* remaining_world = abort_side == 0 ? join_world.get() : host_world.get();
        bool aborted{};
        if (admission_race) {
            stage = "abort races real threaded runtime admission";
            std::barrier start(2);
            std::jthread admission([&] { start.arrive_and_wait(); leaving_world->set_paused(false); });
            start.arrive_and_wait();
            aborted = viewer::try_abort_lan(*leaving, leaving_world);
        } else {
            expect(leaving->request_abort(), "reserve loading abort under match lock");
            // Even if the simulation thread wakes before stop(), its reserved abort
            // must prevent setup admission and the first runtime step.
            leaving_world->set_paused(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            aborted = viewer::try_abort_lan(*leaving, leaving_world);
        }
        if (!aborted) {
            expect(admission_race && leaving->view().phase == viewer::LanPhase::running,
                "admission wins atomically and abort refuses to tear down a running stream");
            expect(!leaving_world->failure(), "refused abort keeps its running driver alive");
            return;
        }
        expect(leaving_world->completed_tick() == 0, "an accepted abort cannot race a first runtime step");
        wait([&] { return remaining->view().phase == viewer::LanPhase::ended; });
        expect(remaining->view().reason == viewer::LanReason::peer_left, "remaining peer receives departure");
        expect(viewer::try_abort_lan(*remaining, remaining_world), "remaining loading peer has an exit");
        expect(host_world->completed_tick() == 0 && join_world->completed_tick() == 0,
            "both peers leave before tick zero without simulation advancement");
        return;
    }
    host_world->set_paused(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    expect(host_world->completed_tick() <= 4 && join_world->completed_tick() == 0,
        "one scene ready cannot run beyond the held peer's sealed horizon");
    join_world->set_paused(false);
    wait([&] { return host_world->failure() || join_world->failure() ||
        (host_world->completed_tick() >= 240 && join_world->completed_tick() >= 240); });
    expect(!host_world->failure() && !join_world->failure(), "threaded viewer facade has no simulation error");
    expect(host_world->snapshot_at(240)->sha256() == join_world->snapshot_at(240)->sha256(),
        "threaded peers publish equal tick 240 snapshots across the monitor boundary");
    expect(!viewer::try_abort_lan(*host, host_world.get()) && !viewer::try_abort_lan(*join, join_world.get()),
        "admitted peers cannot abort outside the agreed quit path");
    // Deterministic admission-wins counterpart to the reserved-abort cases:
    // refusal must leave both drivers and the stream usable for another tick.
    wait([&] { return host_world->completed_tick() >= 241 && join_world->completed_tick() >= 241; });
    expect(!host_world->failure() && !join_world->failure(), "refused abort preserves runtime admission");
}
struct StagingIngress {
    std::shared_ptr<viewer::LanMatch> host{make_match()}, client{make_match()};
    std::unique_ptr<net::TcpTransport> source;
    const std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::now() + std::chrono::seconds(20)};
    StagingIngress() {
        host->configure_seats({1, 2, 1}); host->host(0);
        net::TcpOptions options; options.build_identity = identity().build; options.peers = 3; options.seat = 1;
        source = require(net::TcpTransport::connect("127.0.0.1", host->view().port, options));
        wait([&] { host->pump_setup({}); (void)source->poll(); return source->self() == 1; });
        client->configure_seats({1, 2, 1}); client->join("127.0.0.1:" + std::to_string(host->view().port));
        wait([&] { pump(); return host->view().connected && client->view().connected && source->link(2) == net::LinkState::open; });
    }
    ~StagingIngress() { client->cancel(); host->cancel(); }
    template<class F> void wait(F predicate) {
        while (!predicate()) {
            expect(std::chrono::steady_clock::now() < deadline, "viewer early-frame test deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void pump() { host->pump_setup({}); (void)source->poll(); client->pump_setup({}); }
    void send(const Bytes& frame) {
        auto sent = source->send(2, frame);
        while (!sent && sent.error().code == "EAWR-NET-0205") {
            pump(); expect(std::chrono::steady_clock::now() < deadline, "early-frame backpressure deadline");
            sent = source->send(2, frame);
        }
        require(std::move(sent)); pump();
    }
};
void staging_ingress_bounds() {
    stage = "viewer early hash count and byte bounds";
    eawr::testing::FakeNetwork network(3);
    eawr::testing::FakeTransport source(network, 1);
    net::DesyncMonitor monitor(source);
    auto world = require(tactical::TacticalSession::create(setup(), content()));
    require(monitor.record(0, world.state_sha256(), world.canonical_state_bytes(), {}, false));
    const auto hash = network.inbox[2].front().received.frame;
    for (const bool byte_bound : {false, true}) {
        StagingIngress ingress;
        auto frame = hash;
        if (byte_bound) frame.resize(net::max_batch_bytes);
        const auto count = byte_bound ? 8U : 8192U;
        for (unsigned index = 0; index < count; ++index) {
            ingress.send(frame);
            expect(ingress.client->view().phase != viewer::LanPhase::ended, "early hash at its bound does not end human review");
        }
        ingress.send(hash);
        ingress.wait([&] { ingress.pump(); return ingress.client->view().phase == viewer::LanPhase::ended; });
        expect(ingress.client->view().reason == viewer::LanReason::refused
            && ingress.client->view().message == "Unexpected message while waiting for match setup."
            && !ingress.client->offered_setup(), "one further early hash fails at the count or byte bound without accepting setup");
    }
    stage = "untrusted offer does not change viewer owner";
    StagingIngress ingress;
    auto selected = offer(); selected.seats.push_back({2, sim::SeatId{15}, 1});
    const auto before = ingress.client->view();
    ingress.send(require(net::encode_setup_offer(selected)));
    ingress.wait([&] { ingress.pump(); return ingress.client->view().phase == viewer::LanPhase::ended; });
    expect(!ingress.client->offered_setup() && ingress.client->view().player == before.player
        && ingress.client->view().human_owners == before.human_owners && ingress.client->view().seat == before.seat,
        "a non-host offer is rejected before changing offered setup or local owner");
}
void refused_rosters() {
    stage = "unsupported command-seat roster";
    for (const auto& owners : std::vector<std::vector<tactical::PlayerId>>{
        std::vector<tactical::PlayerId>(10, 1), {1, 2, 3, 4}}) {
        auto match = make_match();
        match->configure_seats(owners); match->host(0);
        expect(match->view().phase == viewer::LanPhase::ended && match->view().diagnostic_code == "EAWR-NET-1300",
            "ten seats and a fourth human owner fail before transport admission");
    }
}
void shared_mesh(std::uint32_t count, bool coop) {
    stage = "shared-owner viewer mesh";
    auto initial = setup();
    initial.players[0].faction_id = eawr::skirmish::faction_id("Rebel");
    initial.players[1].faction_id = eawr::skirmish::faction_id("Empire");
    initial.players[2].faction_id = eawr::skirmish::faction_id("Underworld");
    if (coop) initial.skirmish->slots[1].human = false;
    const auto sides = count == 9 && !coop ? 3U : 2U;
    if (sides == 3) {
        initial.skirmish->slots[2].human = true;
        initial.players[2].faction_id = eawr::skirmish::faction_id("Mod_Fleet_C");
    }
    std::vector<tactical::PlayerId> owners(count, 1);
    if (!coop) for (std::uint32_t peer = 1; peer < count; ++peer) owners[peer] = peer % sides + 1;
    std::vector<std::shared_ptr<viewer::LanMatch>> matches;
    for (std::uint32_t peer = 0; peer < count; ++peer) {
        auto match = make_match(); match->configure_seats(owners); matches.push_back(std::move(match));
    }
    matches[0]->host(0);
    for (std::uint32_t peer = 1; peer < count; ++peer)
        matches[peer]->join("127.0.0.1:" + std::to_string(matches[0]->view().port));
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map;
    selected.slots = std::vector<eawr::skirmish::LobbySlot>{
        {1, "Rebel", 1, true, {"fleet-a"}}, {2, "Empire", 2, !coop, {"fleet-b"}}, {3, sides == 3 ? "Mod_Fleet_C" : "Underworld", 3, sides == 3, {"fleet-ai"}}};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const auto wait = [&](const auto& predicate) {
        while (!predicate()) {
            expect(std::chrono::steady_clock::now() < deadline, "shared viewer mesh deadline");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    wait([&] {
        for (auto& match : matches) match->pump_setup(selected);
        return std::all_of(matches.begin(), matches.end(), [](const auto& match) { return match->view().connected; });
    });
    matches[0]->start(selected);
    expect(matches[0]->take_start().has_value(), "mesh host hands over one shared-faction setup");
    sim::InlineExecutor executor;
    std::vector<tactical::TacticalSession> worlds;
    std::vector<eawr::platform::LiveStepContext> contexts(count);
    std::vector<eawr::platform::LiveStepDriver> drivers(count);
    for (std::uint32_t peer = 0; peer < count; ++peer) worlds.push_back(require(tactical::TacticalSession::create(initial, content())));
    for (std::uint32_t peer = 0; peer < count; ++peer) {
        auto& context = contexts[peer]; context.advance_allowed = false;
        context.completed_tick = [&, peer] { return worlds[peer].completed_tick(); };
        context.state_hash = [&, peer] { return worlds[peer].state_sha256(); };
        context.canonical_state = [&, peer] { return worlds[peer].canonical_state_bytes(); };
        context.install = [&, peer](std::span<const tactical::PlayerCommand> commands) {
            for (const auto& command : commands) require(worlds[peer].submit(command));
            return eawr::core::Result<void>::success();
        };
        context.step = [&, peer] { require(worlds[peer].step(executor)); return eawr::core::Result<void>::success(); };
        context.snapshot = [&, peer] { return worlds[peer].snapshot(); };
    }
    std::uint64_t now{};
    drivers[0] = matches[0]->driver(initial);
    wait([&] {
        ++now;
        for (std::uint32_t peer = 0; peer < count; ++peer) {
            if (drivers[peer]) require(drivers[peer](contexts[peer], {}, now));
            else {
                matches[peer]->pump_setup(selected);
                if (matches[peer]->view().accept_enabled) matches[peer]->accept();
                if (matches[peer]->take_start()) drivers[peer] = matches[peer]->driver(initial);
            }
        }
        return std::all_of(drivers.begin(), drivers.end(), [](const auto& driver) { return !!driver; });
    });
    if (sides == 3) for (std::uint32_t peer = 1; peer < count; ++peer) {
        const auto offered = matches[peer]->offered_setup();
        expect(offered && offered->slots && offered->slots->at(2).faction == "Mod_Fleet_C",
            "joiners decode the third human faction from locally configured mod data");
    }
    for (auto& context : contexts) context.advance_allowed = true;
    wait([&] {
        ++now;
        for (std::uint32_t peer = 0; peer < count; ++peer)
            if (worlds[peer].completed_tick() < 20) require(drivers[peer](contexts[peer], {}, now));
        return std::all_of(worlds.begin(), worlds.end(), [](const auto& world) { return world.completed_tick() == 20; });
    });
    for (std::uint32_t peer = 0; peer < count; ++peer) {
        expect(matches[peer]->local_player() == owners[matches[peer]->view().seat], "local fog and asset owner come from the assigned offer seat");
        expect(matches[peer]->view().human_owners == owners && matches[peer]->view().connected_seats == count,
            "all viewers present the complete human seat roster");
    }
    std::uint32_t leaving = count - 1;
    for (std::uint32_t peer = 1; peer < count; ++peer)
        if (std::count(owners.begin(), owners.end(), matches[peer]->local_player()) > 1) { leaving = peer; break; }
    const tactical::PlayerCommand quit{{20, matches[leaving]->local_player(), 0}, {}, tactical::QuitPayload{}};
    require(drivers[leaving](contexts[leaving], std::span(&quit, 1), ++now));
    wait([&] {
        ++now;
        for (std::uint32_t peer = 0; peer < count; ++peer)
            if (peer == leaving || worlds[peer].completed_tick() < 50) require(drivers[peer](contexts[peer], {}, now));
        bool complete = matches[leaving]->view().phase == viewer::LanPhase::ended;
        for (std::uint32_t peer = 0; peer < count; ++peer) if (peer != leaving)
            complete = complete && worlds[peer].completed_tick() == 50;
        return complete;
    });
    for (std::uint32_t peer = 0; peer < count; ++peer) if (peer != leaving) {
        expect(matches[peer]->view().phase == viewer::LanPhase::running && !matches[peer]->view().victory,
            "a shared controller leaving does not end its faction's battle");
        expect(worlds[peer].state_sha256() == worlds[0].state_sha256() && worlds[peer].record().commands.empty(),
            "surviving shared-owner viewer worlds agree without a faction quit");
    }
    for (auto& match : matches) match->cancel();
}

void shared_builder_matches_former_live_encoding() {
    stage = "real shared skirmish builder parity";
    for (const bool opponent_human : {false, true}) {
        eawr::units::UnitTables tables;
        eawr::skirmish::Fixture fixture;
        fixture.map = "data/art/maps/parity.ted"; fixture.map_sha256 = std::string(64, 'a'); fixture.seed = 83;
        fixture.pre_built_base = fixture.free_starting_units = false;
        fixture.slots = {{1, "Parity_A", 0, true, {}, 2}, {2, "Parity_B", 1, opponent_human, {}, 4}};
        eawr::skirmish::StartInputs inputs; inputs.map = fixture.map; inputs.map_sha256 = fixture.map_sha256;
        inputs.tables = &tables; inputs.lobby_colours.resize(9);
        for (unsigned team = 0; team < 2; ++team) {
            eawr::skirmish::StartFaction faction; faction.name = team == 0 ? "Parity_A" : "Parity_B"; faction.playable = true;
            inputs.factions.push_back(std::move(faction));
            eawr::skirmish::MapPlacement marker; marker.record = team;
            marker.type = "Team_0" + std::to_string(team) + "_Spawn_Point_Marker";
            marker.marker = true; marker.position = sim::math::Vec3{}; marker.orientation_degrees = sim::math::Vec3{};
            inputs.placements.push_back(std::move(marker));
        }
        // Former live path: build_start -> recording_setup -> LAN departure metadata -> queue identities.
        const auto former_start = require(eawr::skirmish::build_start(fixture, inputs));
        auto former_setup = eawr::skirmish::recording_setup(fixture, former_start);
        former_setup.departure_policy.emplace();
        for (const auto& row : former_setup.skirmish->slots) if (row.human) former_setup.departure_policy->humans.push_back(row.player);
        former_setup.queue_identities = true;
        auto shared = require(viewer::prepare_skirmish(fixture, std::move(tables), std::move(inputs)));
        auto moved = std::move(shared);
        expect(moved.inputs.tables == &moved.tables, "shared inputs bind their current owning tables after a move");
        require(eawr::skirmish::build_start(fixture, moved.inputs)); // Exercise the rebound pointer, not only its address.
        const auto shared_setup = viewer::lan_recording_setup(fixture, moved.start);
        expect(require(net::encode_skirmish_setup(former_setup)) == require(net::encode_skirmish_setup(shared_setup)), "shared and former live canonical setup bytes are identical");
        net::SetupOffer former_offer; former_offer.seed = fixture.seed;
        former_offer.setup = require(net::encode_skirmish_setup(former_setup));
        former_offer.identity = {std::string(eawr::core::build_identity()), *former_setup.match_identity};
        former_offer.seats = {{0, sim::SeatId{1}, 1}, {1, sim::SeatId{2}, opponent_human ? 2U : 1U}};
        auto shared_offer = former_offer; shared_offer.setup = require(net::encode_skirmish_setup(shared_setup)); shared_offer.identity.content = *shared_setup.match_identity;
        expect(require(net::encode_setup_offer(former_offer)) == require(net::encode_setup_offer(shared_offer)), "production shared builder matches former live SetupOffer bytes and digest");
    }
}

void lan_ai_defaults_follow_authored_constraints() {
    stage = "LAN AI defaults follow team factions and authored spawn limits";
    eawr::skirmish::SetupMap map; map.path = setup().skirmish->map;
    map.metadata.capacity = 4; map.metadata.new_markers = true; map.metadata.start_positions.emplace(4);
    map.teams = {{0, {"Rebel", "Empire"}, 2}, {1, {"Rebel", "Empire"}, 2}, {2, {"Rebel", "Empire"}, 1}};
    eawr::skirmish::SetupSelection initial; initial.map = map.path; initial.match = setup().skirmish->match;
    initial.slots = {{1, "Rebel", 0, true, {}, 0}, {2, "Empire", 1, true, {}, 1}};
    auto selection = initial;
    require(viewer::append_lan_ai_row(selection, map, 3, 8, true));
    expect(selection.slots.back().team == 2, "a new planned human owner prefers an unused loaded team");
    selection = initial;
    require(viewer::append_lan_ai_row(selection, map, 3, 8));
    expect(selection.slots.back().team == 1 && selection.slots.back().faction == "Empire" && !selection.slots.back().human,
        "new AI keeps the occupied opposing team's faction instead of taking the first allowed faction");
    require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1)));
    map.teams[0].spawn_capacity = 1; map.teams[1].spawn_capacity = 1;
    selection = initial;
    require(viewer::append_lan_ai_row(selection, map, 3, 8));
    expect(selection.slots.back().team == 2, "a full opposing side falls back to an authored side with a valid free spawn");
    map.teams.pop_back(); selection = initial;
    const auto before = require(viewer::encode_lan_lobby_game(require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1)))));
    expect(!viewer::append_lan_ai_row(selection, map, 3, 8), "an AI edit is refused when no side can admit it");
    expect(require(viewer::encode_lan_lobby_game(require(viewer::validated_lan_options(selection, std::span<const eawr::skirmish::SetupMap>(&map, 1))))) == before,
        "a refused AI edit preserves the exact prior selection bytes");
}

void lobby_refusal_and_placeholder_contracts() {
    stage = "typed admission and collision-free temporary names";
    for (const auto& [reason, code] : std::vector<std::pair<net::LobbyReason, std::string>>{
        {net::LobbyReason::full, "EAWR-NET-1604"}, {net::LobbyReason::frozen, "EAWR-NET-1607"},
        {net::LobbyReason::authority, "EAWR-NET-1601"}, {net::LobbyReason::resource_limit, "EAWR-NET-1614"}}) {
        const auto error = net::lobby_diagnostic(reason, "admission refusal");
        const auto mapped = viewer::lan_lobby_reason(error);
        expect(mapped == reason && net::lobby_diagnostic(mapped, "").code == code, "admission reverse mapping preserves the real typed reason");
    }
    eawr::testing::FakeNetwork network(3);
    eawr::testing::FakeTransport transport(network, 0);
    net::LobbyHooks hooks;
    hooks.validate = [](const auto&, bool) { return eawr::core::Result<void>::success(); };
    hooks.offer = [](const auto&, const std::vector<net::SetupSeat>& seats) { auto value = offer(); value.seats = seats; return eawr::core::Result<net::SetupOffer>::success(std::move(value)); };
    hooks.setup = [](net::MessageTransport&, const auto&) { return eawr::core::Result<net::LobbySetupContext>::failure(net::lobby_diagnostic(net::LobbyReason::setup_failed, "unused test provider")); };
    hooks.remove_peer = [](net::PeerIndex, net::LobbyReason) {};
    auto session = require(net::LobbySession::host(transport, "reason-test", 2, {"host", "Rebel", 0, 0, 1}, 1, {setup().skirmish->map, {}}, hooks));
    require(session.admit(1, "first-edge", {"first", "Empire", 1, 1, 2}, 2, session.snapshot().revision));
    const auto full = session.admit(2, "third-edge", {"third", "Empire", 1, 1, 2}, 2, session.snapshot().revision);
    expect(!full && viewer::lan_lobby_reason(full.error()) == net::LobbyReason::full, "actual full-room admission retains 1604");
    require(session.close());
    const auto frozen = session.admit(2, "third-edge", {"third", "Empire", 1, 1, 2}, 2, session.snapshot().revision);
    expect(!frozen && viewer::lan_lobby_reason(frozen.error()) == net::LobbyReason::frozen, "actual frozen-room admission retains 1607");
    net::LobbySnapshot room;
    for (const auto& name : {"2", "2-1", "2-2"}) { net::LobbySeat seat; seat.selection.name = name; room.seats.push_back(std::move(seat)); }
    expect(viewer::lan_admission_name(room, 1) == "2-3", "temporary admission label steps past every occupied name");
}

void terminal_lobby_commands_are_inert() {
    stage = "terminal staging leave and commands";
    const auto initial = setup();
    eawr::skirmish::FixtureOptions selected; selected.map = initial.skirmish->map; selected.match = initial.skirmish->match;
    selected.slots = std::vector<eawr::skirmish::LobbySlot>{{1, "Rebel", 1, true, {}, 2}, {2, "Empire", 2, true, {}, 4}};
    eawr::skirmish::SetupMap map; map.path = *selected.map; map.metadata.capacity = 2;
    map.metadata.new_markers = true; map.metadata.start_positions.emplace(3);
    map.teams = {{1, {"Rebel"}, 1}, {2, {"Empire"}, 1}};
    const viewer::LanLobbyBuilder builder = [initial](const auto&) { return eawr::core::Result<tactical::TacticalSetup>::success(initial); };
    auto host = make_match(), join = make_match();
    host->configure_lobby("Host", 2, {map}, builder); join->configure_lobby("Missing map", 4, {}, builder);
    host->host(0); join->join("127.0.0.1:" + std::to_string(host->view().port));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (join->view().phase != viewer::LanPhase::ended) {
        expect(std::chrono::steady_clock::now() < deadline, "missing-map refusal deadline");
        host->pump_setup(selected); join->pump_setup({}); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(join->view().diagnostic_code == net::lobby_diagnostic(net::LobbyReason::invalid_choice, "").code, "local map refusal has a typed lobby diagnostic");
    join->lobby_leave();
    expect(!join->lobby_ready(true) && !join->lobby_chat("after failure") && !join->lobby_kick(0)
        && !join->lobby_select({"name", "Rebel", 2, 1, 1}) && !join->lobby_game(selected), "terminal lobby mutators cannot touch drain-owned I/O");
    join->cancel(); host->cancel();
}

void close_one_mesh_edge() {
    stage = "targeted close-peer";
    net::TcpOptions policy; policy.peers = 3; policy.build_identity = eawr::core::build_identity();
    auto host = require(net::TcpTransport::host("127.0.0.1", 0, policy));
    require(host->close_peer(1)); // Vacant admission is a no-op.
    expect(!host->close_peer(0), "close_peer refuses self with a typed result");
    auto first = require(net::TcpTransport::connect("127.0.0.1", host->port(), policy));
    auto second = require(net::TcpTransport::connect("127.0.0.1", host->port(), policy));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    const auto fully_open = [&] {
        for (const auto* transport : {host.get(), first.get(), second.get()})
            for (net::PeerIndex peer = 0; peer < transport->peer_count(); ++peer)
                if (transport->link(peer) != net::LinkState::open) return false;
        return true;
    };
    while (!fully_open()) {
        expect(std::chrono::steady_clock::now() < deadline, "close-peer mesh deadline");
        (void)host->poll(); (void)first->poll(); (void)second->poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto removed_identity = host->edge_identity(1);
    auto* removed = first->self() == 1 ? first.get() : second.get();
    require(removed->send(0, Bytes{92}));
    bool before_close = false;
    while (!before_close) {
        expect(std::chrono::steady_clock::now() < deadline, "pre-close frame deadline");
        (void)removed->poll(); (void)(removed == first.get() ? second->poll() : first->poll());
        if (auto frame = host->poll()) { expect(frame->from == 1 && frame->frame == Bytes{92}, "known frame arrives from the soon-removed edge"); before_close = true; }
    }
    require(removed->send(0, Bytes{93})); (void)removed->poll();
    require(host->close_peer(1)); require(host->close_peer(1));
    expect(removed_identity != 0 && host->edge_identity(1) == 0, "only admitted open edge has an identity");
    for (unsigned i = 0; i < 10; ++i) {
        if (auto frame = host->poll()) expect(frame->from != 1, "locally closed peer never yields queued or late application frames");
        (void)removed->poll();
    }
    expect(host->link(1) == net::LinkState::closed && host->link(2) == net::LinkState::open, "ordered close affects exactly the selected edge");
    auto* survivor = first->self() == 2 ? first.get() : second.get();
    require(survivor->send(0, Bytes{94}));
    bool after_close = false;
    while (!after_close) {
        expect(std::chrono::steady_clock::now() < deadline, "post-close live polling deadline");
        (void)survivor->poll(); (void)removed->poll();
        if (auto frame = host->poll()) { expect(frame->from == 2 && frame->frame == Bytes{94}, "only the surviving edge yields application input after close"); after_close = true; }
    }
    require(host->send(2, Bytes{91}));
    bool received = false;
    while (!received) {
        expect(std::chrono::steady_clock::now() < deadline, "surviving edge continues after targeted close");
        (void)host->poll(); (void)(survivor == first.get() ? second->poll() : first->poll());
        if (auto frame = survivor->poll()) received = frame->frame == Bytes{91};
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void missing_planned_owner_is_refused() {
    stage = "three owners on a two-row loaded map";
    auto initial = setup();
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map; selected.match = initial.skirmish->match;
    selected.slots = std::vector<eawr::skirmish::LobbySlot>{{1, "Rebel", 1, true, {}, 0}, {2, "Empire", 2, true, {}, 1}};
    eawr::skirmish::SetupMap map; map.path = *selected.map;
    map.metadata.capacity = 2; map.metadata.new_markers = true; map.metadata.start_positions.emplace(3);
    map.teams = {{1, {"Rebel"}, 1}, {2, {"Empire"}, 1}};
    auto host = make_match(); host->configure_seats({1, 1, 1, 2, 2, 2, 3, 3, 3});
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
    auto initial = setup();
    const std::vector<std::string> factions{"H_mod_A", "H_mod_B", "H_mod_C"};
    eawr::skirmish::FixtureOptions selected;
    selected.map = initial.skirmish->map; selected.match = initial.skirmish->match; selected.seed = initial.seed;
    selected.slots.emplace();
    eawr::skirmish::SetupMap map; map.path = *selected.map;
    map.metadata.capacity = 3; map.metadata.new_markers = true; map.metadata.start_positions.emplace(4);
    initial.departure_policy.emplace(); initial.queue_identities = true;
    for (unsigned row = 0; row < 3; ++row) {
        const bool human = std::find(owners.begin(), owners.end(), row + 1) != owners.end();
        selected.slots->push_back({row + 1, factions[row], row + 1, human, {}, row});
        map.teams.push_back({row + 1, {factions[row]}, 1});
        initial.players[row].faction_id = eawr::skirmish::faction_id(factions[row]);
        initial.players[row].team_id = row + 1;
        initial.skirmish->slots[row].human = human; initial.skirmish->slots[row].fleet.clear();
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
    auto invalid = selected; invalid.slots->push_back({4, "H_mod_A", 1, true, {}, 4});
    expect(!host.lobby_game(invalid), "loaded map capacity of three refuses a fourth owner row");
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

void authoritative_staging(bool play, std::optional<net::LobbyReason> end_during_start = {}) {
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
    host->configure_lobby("Alice", 4, {map}, builder); join->configure_lobby("Bob", 5, {map}, builder);
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
    expect(host->lobby_game(edited), "host changes loaded application options");
    wait([&] { pump(); return !join->lobby_state()->local_ready; });
    expect(!host->view().start_enabled, "consequential options clear both ready icons and Start");
    if (!play) {
        expect(!join->lobby_kick(0), "joiner cannot kick host");
        expect(host->lobby_kick(1), "host kicks admitted joiner");
        wait([&] { pump(); return join->view().phase == viewer::LanPhase::ended; });
        expect(join->lobby_state()->notice && join->lobby_state()->notice->reason == net::LobbyReason::kicked, "typed kick reaches the removed player");
        expect(host->lobby_state()->snapshot.seats.size() == 1 && !host->view().start_enabled, "kick clears readiness and removes exactly one seat");
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

void loopback(bool abrupt, bool departed_ahead = false, bool closed_before_launch = false, int final_window = 0,
    bool speed_during_departure = false, std::shared_ptr<viewer::LanMatch> existing_host = {},
    std::shared_ptr<viewer::LanMatch> existing_join = {}, unsigned workers = 1, bool fail_recovery = false,
    int policy_disagreement = 0) {
        stage = abrupt ? "abrupt connection" : "quit connection";
        auto initial = setup();
        initial.players[0].faction_id = eawr::skirmish::faction_id("Rebel");
        initial.players[1].faction_id = eawr::skirmish::faction_id("Empire");
        initial.players[2].faction_id = eawr::skirmish::faction_id("Underworld");
        auto host = existing_host ? existing_host : make_match({}, 5);
        auto join = existing_join ? existing_join : make_match();
        std::optional<net::TcpRejoinAuth> socket_auth;
        if (final_window < 6) {
            // Cases for departure/resync intentionally negotiate no held-seat rejoin.
            net::SetupRecoveryPolicy recovery; recovery.resync = true;
            host->set_recovery_policy(recovery); join->set_recovery_policy(recovery);
        }
        if (policy_disagreement == 1 || policy_disagreement == 2) {
            net::SetupRecoveryPolicy disabled; disabled.resync = true;
            (policy_disagreement == 1 ? join : host)->set_recovery_policy(disabled);
        }
        host->host(0);
        expect(host->view().port != 0, "host presents the actual listening port");
        expect(host->view().credential_provider_available == (final_window >= 6 && policy_disagreement != 2),
            "credential-provider availability survives host view reset");
        // An application-level TCP relay lets the malformed-frame case use the
        // production handshake and monitor without a test hook in the match.
        std::unique_ptr<net::TcpTransport> relay_host, relay_join;
        if (final_window == 4 || final_window >= 6) {
            net::TcpOptions options; options.build_identity = eawr::core::build_identity();
            relay_host = require(net::TcpTransport::host("127.0.0.1", 0, options));
            relay_join = require(net::TcpTransport::connect("127.0.0.1", host->view().port, options));
        }
        join->join("127.0.0.1:" + std::to_string(relay_host ? relay_host->port() : host->view().port));
        expect(join->view().credential_provider_available == (final_window >= 6 && policy_disagreement != 1),
            "credential-provider availability survives join view reset");
        bool dropped{}, rejoined{}, recovery_seen{}, transient_seen{};
        Bytes credential_delivery, credential_ack;
        bool accept_forwarded{};
        unsigned catchup_batches{}, suppressed_repeats{};
        std::array<std::optional<std::uint64_t>, 2> early_seal_tick;
        std::array<bool, 2> failed{};
        struct PortHold {
            net::socket::Handle handle{net::socket::invalid};
            ~PortHold() { net::socket::close(handle); }
        } occupied;
        const auto relay = [&] {
            if (!relay_host) return;
            if (dropped) { require(relay_host->repair_closed_links(socket_auth)); require(relay_join->repair_closed_links(socket_auth)); }
            for (unsigned i = 0; i < 256; ++i) {
                auto a = relay_join->poll(), b = relay_host->poll();
                if (b && !b->frame.empty() && b->frame[0] == net::lan_credentials_ack_kind) credential_ack = b->frame;
                if (a && !a->frame.empty() && a->frame.front() == net::lan_credentials_kind) {
                    credential_delivery = a->frame;
                    // The transparent trusted-LAN relay retains the real provider's
                    // roster only to authenticate its own replacement edges.
                    expect(a->frame.size() == 74, "real two-seat pair-key delivery");
                    socket_auth.emplace(); socket_auth->epoch = 1;
                    std::copy_n(a->frame.begin() + 6, 32, socket_auth->match_identity.begin());
                    socket_auth->seat_tokens.resize(2);
                    for (auto& key : socket_auth->seat_tokens) std::copy_n(a->frame.begin() + 42, 32, key.begin());
                }
                const auto repeated_early_seal = [&](unsigned peer, const auto& received) {
                    if (!received || !early_seal_tick[peer] || received->frame.empty()
                        || received->frame.front() != net::sealed_batch_kind) return false;
                    const auto batch = require(net::decode_batch(received->frame));
                    if (batch.tick < *early_seal_tick[peer] || batch.tick - *early_seal_tick[peer] >= 2) return false;
                    ++suppressed_repeats; return true;
                };
                const bool hold_delivery = a && !a->frame.empty() && a->frame[0] == net::lan_credentials_kind && policy_disagreement == 3;
                const bool hold_ack = b && !b->frame.empty() && b->frame[0] == net::lan_credentials_ack_kind && policy_disagreement == 5;
                if (a && !hold_delivery && !repeated_early_seal(0, a) && relay_host->link(1) == net::LinkState::open) require(relay_host->send(1, a->frame));
                if (b && !hold_ack && !repeated_early_seal(1, b) && relay_join->link(0) == net::LinkState::open) {
                    require(relay_join->send(0, b->frame));
                    if (!b->frame.empty() && b->frame[0] == net::setup_accept_kind) accept_forwarded = true;
                }
                if (!a && !b) break;
            }
        };
        eawr::skirmish::FixtureOptions selected;
        selected.map = initial.skirmish->map;
        selected.slots = std::vector<eawr::skirmish::LobbySlot>{
            {1, "Rebel", 1, true, {"fleet-a"}}, {2, "Empire", 2, false, {"fleet-b"}}, {3, "Underworld", 3, false, {"fleet-ai"}}};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!host->view().connected || !join->view().connected) {
            expect(std::chrono::steady_clock::now() < deadline, "TCP connection deadline");
            relay(); host->pump_setup(selected); join->pump_setup(selected);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(host->view().start_enabled && !join->view().start_enabled && join->view().read_only,
            "only the connected host starts; joiner is read-only");
        expect(!join->offered_setup() && !join->take_start() && !join->view().accept_enabled,
            "connected joiner waits without presenting local defaults as an offer");
        expect(viewer::lan_message(join->view()) == "Connected - waiting for host setup", "explicit waiting state");
        host->start(selected);
        const auto host_start = host->take_start();
        expect(host_start && host_start->slots->at(1).human && !host_start->slots->at(2).human,
            "two human seats preserve the AI slot");
        initial.departure_policy = tactical::MultiplayerDeparturePolicy{{1, 2}};
        // The termination fixture observes link closure without scheduling any
        // departure-recovery world ticks on its deliberately held peer.
        if (final_window == 3) initial.departure_policy.reset();
        const auto executor = eawr::platform::ThreadWorkerAdapter{workers};
        auto worlds = std::array{require(tactical::TacticalSession::create(initial, content())),
                                require(tactical::TacticalSession::create(initial, content()))};
        std::array<std::vector<std::string>, 2> hashes;
        std::array<std::vector<std::pair<std::uint64_t, std::uint32_t>>, 2> rates;
        std::array<eawr::platform::LiveStepContext, 2> contexts;
        std::array<unsigned, 2> activations{};
        for (std::size_t peer = 0; peer < 2; ++peer) {
            auto& context = contexts[peer];
            context.completed_tick = [&, peer] { return worlds[peer].completed_tick(); };
            context.set_target_rate = [&, peer](std::uint32_t rate) {
                rates[peer].emplace_back(worlds[peer].completed_tick(), rate);
            };
            context.state_hash = [&, peer] {
                if (final_window == 2 && peer == 1 && worlds[peer].completed_tick() == 136)
                    return std::string(64, 'b'); // only the final, non-periodic window diverges
                return worlds[peer].state_sha256();
            };
            context.canonical_state = [&, peer] { return worlds[peer].canonical_state_bytes(); };
            context.install = [&, peer](std::span<const tactical::PlayerCommand> commands) {
                for (const auto& command : commands) { auto accepted = worlds[peer].submit(command); if (!accepted) return accepted; }
                return eawr::core::Result<void>::success();
            };
            context.departures = [&, peer] { const auto controls = worlds[peer].departure_controls();
                return std::vector<tactical::DepartureControl>{controls.begin(), controls.end()}; };
            context.snapshot = [&, peer]() -> std::shared_ptr<const tactical::TacticalSnapshot> {
                if (final_window > 0 && final_window < 5 && worlds[peer].completed_tick() >= 137) {
                    // A synthetic normal victory ends between periodic exchanges,
                    // with both humans still active (unlike LAN quit membership).
                    const tactical::BattleOutcome outcome{tactical::VictoryCondition::enemy_starbase_destroyed, 1, 0, 1, 2, 137};
                    return std::make_shared<tactical::TacticalSnapshot>(worlds[peer].completed_tick(),
                        std::vector<tactical::SnapshotPlayer>{}, std::vector<tactical::TacticalInstance>{},
                        std::vector<tactical::Event>{}, std::vector<tactical::CombatEvent>{},
                        std::vector<tactical::Projectile>{}, outcome);
                }
                return worlds[peer].snapshot();
            };
            context.step = [&, peer] {
                if (final_window == 5 && peer == 1 && !dropped && worlds[peer].completed_tick() == 60) {
                    require(worlds[peer].stage_remove(2)); dropped = true;
                }
                auto tick = worlds[peer].step(executor);
                if (!tick) return eawr::core::Result<void>::failure(tick.error());
                hashes[peer].push_back(worlds[peer].state_sha256()); return eawr::core::Result<void>::success(); };
        }
        if (final_window >= 5) for (std::size_t peer = 0; peer < 2; ++peer) contexts[peer].rebuild = [&, peer] {
            using Context = eawr::platform::LiveStepContext;
            auto staged = std::make_shared<tactical::TacticalSession>(require(tactical::TacticalSession::create(initial, content())));
            auto next = std::make_shared<Context>();
            next->completed_tick = [staged] { return staged->completed_tick(); };
            next->state_hash = [staged] { return staged->state_sha256(); };
            next->canonical_state = [staged] { return staged->canonical_state_bytes(); };
            next->snapshot = [staged] { return staged->snapshot(); };
            next->departures = [staged] { const auto controls = staged->departure_controls();
                return std::vector<tactical::DepartureControl>{controls.begin(), controls.end()}; };
            next->install = [staged](std::span<const tactical::PlayerCommand> commands) {
                for (const auto& command : commands) if (auto installed = staged->submit(command); !installed) return installed;
                return eawr::core::Result<void>::success();
            };
            next->step = [&, peer, staged] {
                if (fail_recovery && staged->completed_tick() == 1) {
                    eawr::core::Diagnostic error; error.code = "EAWR-PLATFORM-0600";
                    error.message = "Synthetic staged replay failure";
                    return eawr::core::Result<void>::failure(std::move(error));
                }
                if (final_window >= 6 && staged->completed_tick() == 1 && catchup_batches == 0) {
                    const auto survivor = 1U - static_cast<unsigned>(peer);
                    early_seal_tick[survivor] = worlds[survivor].completed_tick();
                    for (unsigned count = 0; count < 2; ++count) {
                        // This fixture has no new orders after the hold. Supply its
                        // next two empty survivor seals early, while catch-up runs;
                        // the relay suppresses the survivor's later identical seals,
                        // so successful play requires these catch-up frames to survive.
                        // A gap, duplicate conflict or lost frame prevents resumption.
                        const auto tick = worlds[survivor].completed_tick() + count;
                        const auto frame = require(net::encode_batch({survivor, tick, tick, {}}));
                        require(peer == 0 ? relay_join->send(0, frame) : relay_host->send(1, frame));
                        ++catchup_batches;
                    }
                    relay(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                auto stepped = staged->step(executor);
                return stepped ? eawr::core::Result<void>::success() : eawr::core::Result<void>::failure(stepped.error());
            };
            next->activate = [&, peer, staged] { ++activations[peer]; worlds[peer] = std::move(*staged); return eawr::core::Result<void>::success(); };
            return eawr::core::Result<std::shared_ptr<Context>>::success(std::move(next));
        };
        std::array<eawr::platform::LiveStepDriver, 2> drivers;
        drivers[0] = host->driver(initial);
        std::uint64_t clock = 0;
        if (policy_disagreement) {
            bool injected{}, accepted{};
            unsigned rounds{};
            while (host->view().phase != viewer::LanPhase::ended || join->view().phase != viewer::LanPhase::ended) {
                expect(std::chrono::steady_clock::now() < deadline, "bilateral policy refusal deadline");
                relay();
                (void)drivers[0](contexts[0], {}, clock++);
                if (drivers[1]) (void)drivers[1](contexts[1], {}, clock++);
                else {
                    join->pump_setup(selected);
                    if (join->view().accept_enabled) { join->accept(); accepted = true; }
                    if (join->take_start()) drivers[1] = join->driver(initial);
                }
                if ((policy_disagreement == 4 || policy_disagreement == 6) && !injected && host->view().phase == viewer::LanPhase::running
                    && join->view().phase == viewer::LanPhase::running) {
                    if (policy_disagreement == 4) require(relay_host->send(1, credential_delivery));
                    else {
                        expect(credential_ack.size() == 37, "real admitted-seat acknowledgement captured before cap test");
                        for (unsigned frame = 0; frame < 256; ++frame) require(relay_join->send(0, credential_ack));
                        // Each transport pump flushes at most 64 frames. Put the
                        // whole consecutive burst on the stream before its owner polls.
                        for (unsigned flush = 0; flush < 4; ++flush) (void)relay_join->poll();
                    }
                    injected = true;
                }
                if (policy_disagreement == 5 && ++rounds == 100) {
                    expect(accepted && accept_forwarded && static_cast<bool>(drivers[1]), "missing-ack fixture reached real setup acceptance");
                    expect(host->view().phase != viewer::LanPhase::running && join->view().phase != viewer::LanPhase::running
                        && worlds[0].completed_tick() == 0 && worlds[1].completed_tick() == 0,
                        "missing real delivery ack prevents Start despite setup acceptance");
                    host->cancel(); join->cancel(); return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (policy_disagreement != 4 && policy_disagreement != 6) expect(worlds[0].completed_tick() == 0 && worlds[1].completed_tick() == 0,
                "policy disagreement never advances either world");
            else expect(injected, "stray credential frame was sent during a live match");
            expect(!host->view().rejoin_enabled && !join->view().rejoin_enabled, "refused seats cannot rejoin");
            for (const auto& view : {host->view(), join->view()}) {
                const auto expected = policy_disagreement == 3 ? "EAWR-NET-1023" : policy_disagreement == 4 ? "EAWR-NET-1024"
                    : policy_disagreement == 6 ? "EAWR-NET-1026" : "EAWR-NET-1025";
                expect(view.diagnostic_code == expected, "both peers retain the typed credential refusal");
                expect(view.message.find("credential") != std::string::npos || view.message.find("provider") != std::string::npos || view.message.find("policy") != std::string::npos,
                    "both players see the credential or policy refusal reason");
            }
            host->cancel(); join->cancel(); return;
        }
        while (!drivers[1]) {
            expect(std::chrono::steady_clock::now() < deadline, "offer handover deadline");
            relay();
            require(drivers[0](contexts[0], {}, clock++));
            join->pump_setup(selected);
            if (join->view().accept_enabled) {
                expect(join->offered_setup() && !join->take_start(), "review never launches before Accept");
                expect(join->view().setup_summary.find("fleet-ai") != std::string::npos &&
                    join->view().setup_summary.find("Content:") != std::string::npos, "canonical fleet and identity are presented");
                for (unsigned review = 0; review < 8; ++review) {
                    relay(); require(drivers[0](contexts[0], {}, clock++)); join->pump_setup(selected);
                    expect(worlds[0].completed_tick() == 0 && !join->take_start(), "review holds the host at tick zero");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                clock += 180001;
                require(drivers[0](contexts[0], {}, clock++));
                expect(worlds[0].completed_tick() == 0, "acknowledged human review has no setup deadline");
                join->accept();
                if (closed_before_launch) {
                    host->cancel();
                    while (join->view().phase != viewer::LanPhase::ended) {
                        expect(std::chrono::steady_clock::now() < deadline, "review disconnect deadline");
                        join->pump_setup(selected);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    expect(join->view().reason == viewer::LanReason::peer_left && !join->take_start(),
                        "connection failure after Accept blocks the queued scene launch");
                    expect(worlds[0].completed_tick() == 0 && worlds[1].completed_tick() == 0,
                        "failed review never advances a world");
                    return;
                }
            }
            if (auto start = join->take_start()) {
                expect(start->slots && start->slots->size() == 3 && start->slots->at(1).human,
                    "joiner rebuilds human and AI roster from the buffered offer");
                drivers[1] = join->driver(initial);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stage = "running to tick 120";
        std::array<bool, 2> sent{};
        std::array<std::uint64_t, 2> clicks{};
        std::array<std::shared_ptr<eawr::presentation::ui::CommandScheduler>, 2> schedulers;
        for (std::size_t peer = 0; peer < 2; ++peer) {
            schedulers[peer] = std::make_shared<eawr::presentation::ui::CommandScheduler>(
                static_cast<tactical::PlayerId>(peer + 1), 0, 0,
                [&, peer] { return contexts[peer].completed_tick(); });
            (peer == 0 ? host : join)->set_input_epoch_provider([scheduler = std::weak_ptr(schedulers[peer])] {
                const auto producer = scheduler.lock();
                return producer ? producer->pending_intent_epoch() : std::nullopt;
            });
            (peer == 0 ? host : join)->set_discard_observer([scheduler = std::weak_ptr(schedulers[peer])](std::uint64_t sequence) {
                if (const auto producer = scheduler.lock()) producer->present_result(sequence);
            });
        }
        bool monitor_stall_seen{};
        bool speed_sent{};
        const auto goal = final_window >= 5 ? 180U : 120U;
        while (worlds[0].completed_tick() < goal || worlds[1].completed_tick() < goal
            || (final_window >= 5 && !recovery_seen)) {
            if (final_window >= 6 && !dropped && worlds[0].completed_tick() >= 60 && worlds[1].completed_tick() >= 60) {
                relay_host->cancel(); relay_join->cancel(); dropped = true;
                // Let the two real streams observe EOF before rebuilding the relays.
                require(drivers[0](contexts[0], {}, clock++)); require(drivers[1](contexts[1], {}, clock++));
                if (final_window == 8) {
                    occupied.handle = net::socket::listen("127.0.0.1", host->view().port).handle;
                    expect(occupied.handle != net::socket::invalid, "temporarily occupy the repair listener port");
                }
            }
            if (final_window == 8 && occupied.handle != net::socket::invalid && host->view().diagnostic_code == "EAWR-NET-1017") {
                expect(host->view().phase == viewer::LanPhase::reconnecting, "transient repair failure keeps the seat held");
                net::socket::close(occupied.handle); occupied.handle = net::socket::invalid;
                transient_seen = true; clock += 1000;
            }
            if (final_window >= 6 && dropped && !rejoined && host->view().rejoin_enabled && join->view().rejoin_enabled) {
                auto returning = final_window == 7 ? host : join;
                expect(returning->view().recovery == viewer::LanRecovery::held
                    && viewer::lan_message(returning->view()).starts_with("Connection lost. Reconnecting..."), "held seat overlay");
                if (final_window == 6 || final_window == 7) {
                    const std::size_t peer = final_window == 7 ? 0U : 1U;
                    expect(!returning->request_speed(2), "held recovery refuses shared speed admission");
                    auto& producer = *schedulers[peer]; producer.present_snapshot(worlds[peer].completed_tick());
                    eawr::presentation::ui::TacticalIntent pending; pending.verb = eawr::presentation::ui::TacticalVerb::beacon;
                    require(producer.issue(pending));
                    auto stale = producer.take(worlds[peer].completed_tick());
                    expect(producer.reservations().size() == 1, "accepted-before-hold input retains its UI reservation");
                    require(drivers[peer](contexts[peer], stale, clock++));
                    expect(producer.reservations().empty() && returning->view().input_feedback.find("cancelled") != std::string::npos,
                        "real held drain retires dropped reservations and reports cancellation across worker counts");
                }
                expect(returning->rejoin(), "production rejoin action accepts the held seat");
                if (final_window == 9) {
                    // Consume no further driver poll before cancellation. The next match
                    // reuses both production match owners, so stale intent would tear it down.
                    host->cancel(); join->cancel();
                    expect(!host->rejoin() && !join->rejoin(), "cancel clears rejoin admission");
                    loopback(false, false, false, 0, false, host, join);
                    return;
                }
                rejoined = true; clock += 1000;
            }
            recovery_seen = recovery_seen || host->view().phase == viewer::LanPhase::resyncing
                || join->view().phase == viewer::LanPhase::resyncing || rejoined;
            if (!speed_sent && worlds[0].completed_tick() >= 45 && worlds[1].completed_tick() >= 45) {
                expect(host->request_speed(0) && join->request_speed(4), "both humans request shared speed");
                if (!speed_during_departure)
                    expect(host->request_speed(1) && !host->request_speed(2), "third local change in window is refused");
                expect(!host->request_speed(5), "invalid speed refused locally");
                expect(host->view().options.tactical_speed == 2 && join->view().options.tactical_speed == 2,
                    "request cannot mutate local pacing before future admission");
                speed_sent = true;
            }
            expect(std::chrono::steady_clock::now() < deadline, "lockstep deadline");
            relay();
            for (std::size_t peer = 0; peer < 2; ++peer) {
                if (failed[peer]) continue;
                if (final_window < 5 && !monitor_stall_seen && worlds[0].completed_tick() == 120 && worlds[1].completed_tick() == 119) {
                    clock += 3001;
                    require(drivers[0](contexts[0], {}, clock));
                    expect(viewer::lan_message(host->view()).find("Waiting for Opponent") != std::string::npos,
                        "a stalled hash exchange exposes the same delayed overlay");
                    monitor_stall_seen = true;
                }
                contexts[peer].advance_allowed = worlds[peer].completed_tick() < (speed_sent ? goal : 45U)
                    && (final_window >= 5 || peer == 0 || monitor_stall_seen || worlds[peer].completed_tick() < 119);
                if (final_window >= 6 && rejoined && peer == (final_window == 7 ? 0U : 1U) && activations[peer] == 0)
                    contexts[peer].advance_allowed = false; // Publication must not wait for the next due tick.
                std::vector<tactical::PlayerCommand> commands;
                if (!sent[peer] && host->view().phase == viewer::LanPhase::running && join->view().phase == viewer::LanPhase::running)
                    {
                        // Exercise the real facade after a held pump has visited
                        // this epoch. It must still admit its C+D input slot.
                        clicks[peer] = contexts[peer].completed_tick();
                        (void)schedulers[peer]->take(clicks[peer]);
                        const auto advance = contexts[peer].advance_allowed;
                        contexts[peer].advance_allowed = false;
                        expect(!require(drivers[peer](contexts[peer], {}, clock)), "held facade pump never advances");
                        contexts[peer].advance_allowed = advance;
                        eawr::presentation::ui::TacticalIntent intent;
                        intent.verb = eawr::presentation::ui::TacticalVerb::move;
                        intent.units = {peer + 1};
                        intent.destination = {units(20), units(20), {}};
                        require(schedulers[peer]->issue(intent));
                        sent[peer] = true;
                    }
                // Join drains one simulation frame later; host drains immediately.
                // Both must retain the producer's completed frame as the LAN anchor.
                if (schedulers[peer]->pending() && (peer == 0 || worlds[peer].completed_tick() > clicks[peer])) {
                    commands = schedulers[peer]->take(worlds[peer].completed_tick());
                    expect(commands.size() == 1 && commands.front().key.tick == clicks[peer],
                        "LAN UI drain preserves its producer epoch");
                }
                auto result = drivers[peer](contexts[peer], commands, clock);
                if (!result && fail_recovery) {
                    failed[peer] = true;
                    const auto view = (peer == 0 ? host : join)->view();
                    expect(view.phase == viewer::LanPhase::ended && view.reason == viewer::LanReason::desync
                        && view.desync_tick && !view.bundle_path.empty(),
                        "failed single resync ends with the desync result and saved bundle path");
                    expect(viewer::lan_message(view).find(view.bundle_path) != std::string::npos,
                        "desync result copy exposes the bundle path after failed resync");
                    expect(view.recovery == viewer::LanRecovery::failed && !viewer::lan_gameplay_enabled(view),
                        "real failed resync disables gameplay and has failed recovery presentation");
                    const auto match = peer == 0 ? host : join;
                    expect(view.diagnostic_available && match->save_diagnostic_details(), "real local sink retains saved bundle after terminal failure");
                } else require(std::move(result));
            }
            if (fail_recovery && failed[0] && failed[1]) {
                const auto state = worlds[0].state_sha256() + worlds[1].state_sha256();
                if (workers == 1) recovery_worker_hashes[50] = state;
                else expect(recovery_worker_hashes.at(50) == state, "failed recovery state equal across 1/2/4/8 workers");
                host->cancel(); join->cancel(); return;
            }
            ++clock;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (final_window >= 5) {
            expect(!fail_recovery, "injected resync failure must reach both terminal views");
            expect(dropped && recovery_seen && (final_window == 5 || rejoined), "real TCP recovery path exercised");
            expect(final_window != 8 || transient_seen, "transient repair retries before eventual recovery");
            expect(final_window == 5 || (catchup_batches == 2 && suppressed_repeats == 2),
                "survivor catch-up batches are required: their later duplicate sends were suppressed");
            const auto returning = final_window == 7 ? 0U : 1U;
            expect(activations[returning] == 1, "proved recovery publishes exactly once even while presentation holds advancement");
            expect(worlds[0].state_sha256() == worlds[1].state_sha256() && worlds[0].record() == worlds[1].record(),
                "repaired viewer worlds retain equal final hashes and replay commands");
            expect(host->view().phase == viewer::LanPhase::running && join->view().phase == viewer::LanPhase::running,
                "recovery returns both battle HUDs to play");
            for (const auto& match : {host, join}) {
                const auto view = match->view();
                if (final_window == 8 && view.active_adapter_publications == 0) {
                    const auto text = viewer::lan_message(view);
                    expect(view.phase == viewer::LanPhase::running && view.recovery == viewer::LanRecovery::none
                        && viewer::lan_gameplay_enabled(view) && !view.desync_tick
                        && text.find("Game synchronized.") == std::string::npos && text.find("Incident at tick") == std::string::npos,
                        "transient-repair survivor resumes ordinary play with no stale recovery notice");
                } else {
                    if (view.recovery != viewer::LanRecovery::recovered || !viewer::lan_gameplay_enabled(view))
                        std::cerr << "recovery view case=" << final_window << " workers=" << workers << " phase=" << static_cast<int>(view.phase)
                            << " recovery=" << static_cast<int>(view.recovery) << " gameplay=" << viewer::lan_gameplay_enabled(view)
                            << " publications=" << view.active_adapter_publications << '\n';
                    expect(view.recovery == viewer::LanRecovery::recovered && viewer::lan_gameplay_enabled(view),
                        "real recovery resumes presentation and gameplay across 1/2/4/8 workers");
                }
                expect(view.grace_ms == 30000 && view.progress_timeout_ms == 30000 && view.hard_cap_ms == 300000,
                    "real recovery presentation retains negotiated policy across worker counts");
            }
            expect((returning == 0 ? host : join)->view().active_adapter_publications == activations[returning],
                "recovered HUD and controller context follow published active_adapter across worker counts");
            if (final_window >= 6) expect(socket_auth.has_value(), "real recovery captured provider delivery");
            if (socket_auth) for (const auto& token : socket_auth->seat_tokens) {
                for (const auto& world : worlds) {
                    const auto canonical = world.canonical_state_bytes();
                    const auto replay = require(tactical::write_replay(world.record()));
                    for (const auto& bytes : {canonical, replay})
                        expect(std::search(bytes.begin(), bytes.end(), token.begin(), token.end()) == bytes.end(),
                            "real credentials excluded from world and replay after rejoin");
                }
                for (const auto& view : {host->view(), join->view()})
                    for (const auto& text : {view.message, view.setup_summary, view.bundle_path, view.diagnostic_code})
                        expect(std::search(text.begin(), text.end(), token.begin(), token.end(),
                            [](char character, std::uint8_t byte) { return static_cast<std::uint8_t>(character) == byte; }) == text.end(),
                            "real credentials excluded from viewer diagnostics and UI");
            }
            const auto state = worlds[0].state_sha256();
            if (workers == 1) recovery_worker_hashes[final_window] = state;
            else expect(recovery_worker_hashes.at(final_window) == state, "recovery state equal across 1/2/4/8 workers");
            std::cout << "viewer recovery case=" << final_window << " workers=" << workers << " state=" << worlds[0].state_sha256() << '\n';
            host->cancel(); join->cancel(); return;
        }
        expect(monitor_stall_seen, "hash exchange stall path exercised");
        expect(hashes[0] == hashes[1] && hashes[0].size() == 120, "two viewer facades advance the existing worlds with identical hashes");
        expect(speed_sent && rates[0] == rates[1] && !rates[0].empty(), "speed choices apply at identical ticks/rates on both peers");
        expect(rates[0].front().first > 45, "speed event executes at a future tick");
        expect(host->view().options == join->view().options && host->view().options.tactical_speed == 4,
            "later canonical join seat wins the speed choices");
        expect(host->local_player() == 1 && join->local_player() == 2, "each facade binds its own human faction");
        expect(host->view().input_delay == 5 && join->view().input_delay == 5, "monitor and core use the non-default agreed delay");
        expect(sent[0] && sent[1], "both local seat inputs exercised");
        for (const auto& world : worlds) {
            const auto replay = world.record();
            expect(replay.commands.size() == 2, "facade records both human clicks once");
            for (const auto& command : replay.commands) {
                const auto origin = static_cast<std::size_t>(command.key.player_id - 1);
                expect(origin < clicks.size(), "facade latency input has a human origin");
                if (origin < clicks.size()) expect(command.key.tick + 1 - clicks[origin] == 6,
                    "real LAN facade click executes at completed frame C+D+1 for delay 5");
            }
        }
        stage = "draining tick 120 hash exchange";
        contexts[0].advance_allowed = contexts[1].advance_allowed = false;
        for (unsigned i = 0; i < 8; ++i) {
            relay();
            for (std::size_t peer = 0; peer < 2; ++peer) require(drivers[peer](contexts[peer], {}, clock));
            ++clock;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (final_window) {
            stage = "normal victory final-window TCP exchange on separate owner threads";
            contexts[0].advance_allowed = contexts[1].advance_allowed = true;
            bool left{}, delayed{};
            while (host->view().connected || join->view().connected) {
                expect(std::chrono::steady_clock::now() < deadline, "final-window close deadline");
                relay();
                contexts[0].advance_allowed = worlds[0].completed_tick() < 136
                    || worlds[1].completed_tick() >= 136;
                require(drivers[0](contexts[0], {}, clock));
                if ((final_window == 2 || final_window == 3) && !delayed &&
                    host->view().phase == viewer::LanPhase::ended) {
                    stage = "ended host retains final exchange beyond the teardown timeout";
                    expect(worlds[1].completed_tick() == 136, "hold peer before final summary");
                    const auto release = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
                    while (std::chrono::steady_clock::now() < release) {
                        require(drivers[0](contexts[0], {}, clock++));
                        expect(host->view().connected, "no teardown deadline while the end panel owns I/O");
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    delayed = true;
                }
                if (final_window == 4 && host->view().phase == viewer::LanPhase::ended) {
                    stage = "malformed post-end summary produces a typed failure";
                    expect(worlds[1].completed_tick() == 136, "malformed frame arrives before peer final summary");
                    const std::array<std::uint8_t, 1> malformed{net::desync_combined_kind};
                    require(relay_join->send(0, malformed));
                    (void)relay_join->poll();
                    while (host->view().connected) {
                        expect(std::chrono::steady_clock::now() < deadline, "malformed final-frame deadline");
                        auto result = drivers[0](contexts[0], {}, clock++);
                        if (!result) {
                            expect(result.error().code == "EAWR-NET-0401", "ended driver preserves monitor diagnostic code");
                            expect(host->view().reason != viewer::LanReason::desync && !host->view().desync_tick &&
                                host->view().bundle_path.empty(), "protocol failure does not invent a divergence report");
                            expect(host->view().message.find(result.error().code) != std::string::npos,
                                "terminal view retains typed malformed-frame diagnostic");
                            expect(viewer::try_abort_lan(*host, nullptr), "protocol failure allows terminal cleanup");
                            expect(host->view().reason != viewer::LanReason::desync &&
                                host->view().message.find(result.error().code) != std::string::npos,
                                "cleanup does not relabel a protocol failure as an unreported desync");
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    expect(false, "malformed final frame must fail the ended driver");
                }
                if (final_window == 3 && host->view().phase == viewer::LanPhase::ended) {
                    stage = "viewer termination with an unresponsive final-window drain in flight";
                    expect(worlds[1].completed_tick() == 136, "peer has not produced its final summary");
                    expect(viewer::try_abort_lan(*host, nullptr), "UI hands off the incomplete final exchange");
                    const auto began = std::chrono::steady_clock::now();
                    // The peer does not pump or acknowledge close during shutdown.
                    // This is the same seam invoked before extension unload.
                    viewer::shutdown_lan_drains();
                    const auto elapsed = std::chrono::steady_clock::now() - began;
                    expect(elapsed >= std::chrono::milliseconds(1500),
                        "Leave after a long end panel starts a fresh bounded drain");
                    expect(elapsed < std::chrono::seconds(3),
                        "viewer termination bounds an unresponsive drain, then joins its worker");
                    expect(worlds[0].completed_tick() == 137 && worlds[1].completed_tick() == 136,
                        "termination never advances an ended world or its unresponsive peer");
                    // The held facade diagnoses a closed link only when it needs
                    // another seal/hash voter. Resume it AFTER the shutdown assertion
                    // to observe EOF; this is peer recovery, not drain advancement.
                    contexts[1].advance_allowed = true;
                    while (join->view().connected) {
                        expect(std::chrono::steady_clock::now() < deadline, "terminated drain releases its real socket");
                        (void)drivers[1](contexts[1], {}, clock++);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    expect(worlds[0].completed_tick() == 137, "released host world remains ended during peer recovery");
                    expect(join->view().phase == viewer::LanPhase::ended,
                        "peer observes the socket released before either match is destroyed");
                    return;
                }
                if (final_window == 1 && !left && host->view().phase == viewer::LanPhase::ended) {
                    // The joiner has not reached the end yet. UI cleanup must return
                    // without waiting for it, while its final exchange still arrives.
                    expect(worlds[1].completed_tick() < 137, "peer is held behind the final tick at UI exit");
                    const auto began = std::chrono::steady_clock::now();
                    expect(viewer::try_abort_lan(*host, nullptr), "terminal UI exit accepted");
                    expect(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(250),
                        "terminal UI exit does not wait for the peer's drain");
                    left = true;
                }
                // Hold the second final tick until A is visibly ended. This
                // deterministically exercises a summary arriving after phase ended.
                contexts[1].advance_allowed = worlds[1].completed_tick() < 136
                    || host->view().phase == viewer::LanPhase::ended;
                std::exception_ptr failure;
                {
                    std::jthread peer([&] {
                        try { require(drivers[1](contexts[1], {}, clock)); }
                        catch (...) { failure = std::current_exception(); }
                    });
                }
                if (failure) std::rethrow_exception(failure);
                ++clock;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            expect(worlds[0].completed_tick() == 137 && worlds[1].completed_tick() == 137,
                "ended drainage never advances either world");
            if (final_window == 2) {
                for (const auto& match : {host, join}) {
                    expect(match->view().reason == viewer::LanReason::desync, "final-only divergence is reported on both peers");
                    expect(match->view().desync_tick == 136 && !match->view().bundle_path.empty(),
                        "final rows locate the divergent tick and publish diagnostics before close");
                }
            } else {
                expect(left && join->view().victory == false && join->view().reason != viewer::LanReason::desync,
                    "non-blocking exit delivers the equal final exchange and preserves the peer result");
            }
            return;
        }
        if (departed_ahead) {
            stage = "departing peer at maximum lead";
            contexts[0].advance_allowed = false;
            contexts[1].advance_allowed = true;
            while (worlds[1].completed_tick() < 125) {
                expect(std::chrono::steady_clock::now() < deadline, "maximum lead deadline");
                for (std::size_t peer = 0; peer < 2; ++peer) require(drivers[peer](contexts[peer], {}, clock));
                ++clock;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            expect(worlds[0].completed_tick() == 120 && worlds[1].completed_tick() == 125,
                "departing peer reaches the held survivor's sealed LAN horizon D");
            contexts[1].advance_allowed = false;
            // Flush the departing peer's new seal horizon before closing the real stream.
            for (unsigned i = 0; i < 8; ++i) {
                for (std::size_t peer = 0; peer < 2; ++peer) require(drivers[peer](contexts[peer], {}, clock));
                ++clock;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            contexts[0].advance_allowed = true;
        } else {
            stage = "completeness stall";
            contexts[0].advance_allowed = true;
            // A stalled presentation peer must produce MP-33 after 3s of completeness wait.
            for (unsigned i = 0; i < 30; ++i) require(drivers[0](contexts[0], {}, clock++));
            clock += 3001;
            require(drivers[0](contexts[0], {}, clock++));
            expect(viewer::lan_message(host->view()).find("Waiting for Opponent") != std::string::npos,
                "production facade exposes the delayed completeness overlay");
            contexts[1].advance_allowed = true;
        }
        if (abrupt) {
            stage = "abrupt disconnect recovery";
            if (speed_during_departure) {
                // With two peers a detected close can settle in the same pump.
                // Expiry leaves agreement pending until the next pump; close the
                // stream during that agreement and drain a simultaneous UI request.
                clock += 30'001;
                require(drivers[0](contexts[0], {}, clock++));
                expect(host->view().phase == viewer::LanPhase::running && !host->view().departure,
                    "departure agreement holds admission while match remains running");
                expect(host->request_speed(3), "UI queues speed during departure agreement");
            }
            join->cancel(); // Abort the real ordered TCP stream without a quit frame.
            while (!host->view().departure) {
                expect(std::chrono::steady_clock::now() < deadline, "disconnect result deadline");
                require(drivers[0](contexts[0], {}, clock++));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            expect(host->view().departure->event.reason == (speed_during_departure ? net::DepartureReason::timeout
                : net::DepartureReason::disconnect), "typed opponent departure retains its agreed reason");
            if (speed_during_departure) {
                expect(host->view().reason != viewer::LanReason::refused && host->view().options.tactical_speed == 4,
                    "refused speed admission is dropped without ending departure recovery");
                expect(rates[0] == rates[1], "dropped request never changes local pacing");
            }
            expect(host->view().departure->state == net::DepartureState::won_by_departure && host->view().victory == true,
                "the remaining human wins even when an AI slot exists");
            if (departed_ahead) expect(host->view().departure->event.key.tick == 130,
                "maximum-lead LAN departure recovers the full 2D sealed-input frontier");
            return;
        }
        stage = "intentional quit agreement";
        const tactical::PlayerCommand quit{{120, 1, 1}, {}, tactical::QuitPayload{}};
        require(drivers[0](contexts[0], std::span(&quit, 1), clock++));
        while (!host->view().departure || !join->view().departure) {
            expect(std::chrono::steady_clock::now() < deadline, "quit result deadline");
            for (std::size_t peer = 0; peer < 2; ++peer) require(drivers[peer](contexts[peer], {}, clock));
            ++clock;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(host->view().departure->state == net::DepartureState::match_ended &&
            join->view().departure->state == net::DepartureState::won_by_departure, "per-seat typed quit outcomes");
        expect(host->view().victory == false && join->view().victory == true, "quitter loses and opponent wins");
        expect(!viewer::lan_escape_aborts(host->view()) && !viewer::lan_escape_aborts(join->view()),
            "real decided LAN peers route Escape to their end panels");
        expect(host->view().departure->event.reason == net::DepartureReason::quit, "intentional departure is typed");
        expect(hashes[0] == hashes[1], "quit tick is agreed and replayed identically");
        const auto result = host->view();
        stage = "ended match drains and releases TCP before view destruction";
        const auto end_ticks = std::array{worlds[0].completed_tick(), worlds[1].completed_tick()};
        while (host->view().connected || join->view().connected) {
            expect(std::chrono::steady_clock::now() < deadline, "ordered result close deadline");
            for (std::size_t peer = 0; peer < 2; ++peer) require(drivers[peer](contexts[peer], {}, clock));
            ++clock;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(worlds[0].completed_tick() == end_ticks[0] && worlds[1].completed_tick() == end_ticks[1],
            "close drainage never steps an ended world");
        expect(host->view().departure->event.reason == net::DepartureReason::quit &&
            join->view().departure->event.reason == net::DepartureReason::quit,
            "ordered close cannot replace the final quit with a disconnect");
        expect(viewer::try_abort_lan(*host, nullptr), "ended result cleanup succeeds");
        expect(host->view().phase == result.phase && host->view().victory == result.victory &&
            host->view().message == result.message && host->view().departure->state == result.departure->state,
            "terminal cleanup retains departure and result diagnostics");
}
}

// Ticket E: long sessions. Synthetic tick counts only; nothing here waits for real hours.
static void long_session_contracts() {
    constexpr std::uint64_t rate = tactical::logical_frames_per_second;
    const auto ticks_after = [](std::uint64_t hours) { return hours * 3600U * rate; };
    // The net cap equals the simulation's own cap: one agreed final tick count bounds both, so a session can
    // never be offered more ticks than the sim, its replay and its checkpoints can hold.
    expect(net::max_session_ticks == tactical::max_ticks, "net session cap equals the tactical sim cap");
    expect(ticks_after(1) < net::max_session_ticks && ticks_after(4) < net::max_session_ticks, "1 h and 4 h of play fit under the cap");
    expect(ticks_after(12) > net::max_session_ticks && net::max_session_ticks / (3600U * rate) == 9, "12 h of play exceeds it: the cap is 9 h 15 min at the logical rate");
    for (const auto ticks : {std::uint64_t{1}, ticks_after(1), ticks_after(4), net::max_session_ticks}) {
        auto selected = offer(); selected.final_tick_count = ticks;
        expect(static_cast<bool>(net::encode_setup_offer(selected)), "an offer up to the cap is encodable");
    }
    for (const auto ticks : {std::uint64_t{0}, ticks_after(12), net::max_session_ticks + 1}) {
        auto selected = offer(); selected.final_tick_count = ticks;
        expect(!net::encode_setup_offer(selected), "an offer outside 1..cap is refused before it reaches a peer");
    }
    // Match-length warning: silent until 15 minutes before the end, then a curated message with a typed code.
    const auto end = net::max_session_ticks;
    expect(viewer::match_length_notice(end - 15U * 60U * rate - 1U, end).empty(), "no length warning before the last 15 minutes");
    const auto warning = viewer::match_length_notice(end - 15U * 60U * rate, end);
    expect(warning.find("15 min") != std::string::npos && warning.find("EAWR-NET-1738") != std::string::npos, "length warning names minutes and the code");
    expect(viewer::match_length_notice(end - 1, end).find("1 min") != std::string::npos, "last tick rounds up to one minute");
    expect(viewer::match_length_notice(end, end).empty() && viewer::match_length_notice(0, 0).empty(), "no warning at or after the end");
    // Service lease warning: a renewable lease is silent until its last two minutes; a final lease warns earlier.
    const auto now = std::chrono::steady_clock::now();
    net::GnsSessionLease lease; lease.renewable = true; lease.ends = now + std::chrono::minutes(30);
    expect(viewer::online_lease_notice({}, now).empty() && viewer::online_lease_notice(lease, now).empty(), "healthy renewable lease shows nothing");
    lease.ends = now + std::chrono::seconds(90);
    expect(viewer::online_lease_notice(lease, now).find("EAWR-NET-1735") != std::string::npos, "renewal in progress is shown near the end");
    lease.last_refusal = "EAWR-NET-1733";
    expect(viewer::online_lease_notice(lease, now).find("EAWR-NET-1733") != std::string::npos, "failed renewal is shown with its code");
    lease.ends = now + std::chrono::minutes(10);
    expect(viewer::online_lease_notice(lease, now).find("EAWR-NET-1733") != std::string::npos, "failed renewal is visible before the last two minutes");
    lease.last_refusal.clear();
    lease.renewable = false; lease.ends = now + std::chrono::minutes(40);
    expect(viewer::online_lease_notice(lease, now).empty(), "final lease is quiet while far from its end");
    lease.ends = now + std::chrono::minutes(10);
    const auto final_notice = viewer::online_lease_notice(lease, now);
    expect(final_notice.find("10 min") != std::string::npos && final_notice.find("EAWR-NET-1730") != std::string::npos, "final lease warns with minutes and the code");
    for (const bool renewable : {false, true}) {
        lease.renewable = renewable; lease.ends = now - std::chrono::seconds(1);
        const auto ended = viewer::online_lease_notice(lease, now, renewable ? "EAWR-NET-1731" : "EAWR-NET-1730");
        expect(ended.find("service session has ended") != std::string::npos && ended.find("being renewed") == std::string::npos,
            "A stale lease and failed endpoint never promise renewal");
        expect(ended.find(renewable ? "EAWR-NET-1731" : "EAWR-NET-1730") != std::string::npos, "Ended session preserves its curated ending code");
        expect(viewer::online_lease_notice(lease, now).find("has ended") != std::string::npos, "Expiry alone ends the notice even before endpoint failure");
    }
    lease.ends = now + std::chrono::minutes(30);
    expect(viewer::online_lease_notice(lease, now, "EAWR-NET-1528").find("has ended") != std::string::npos,
        "Endpoint failure is visible even when the stale lease still has time left");
    const auto paired = now - std::chrono::hours(24);
    net::GnsTurnCredentials turn{"turn:127.0.0.1:3478", "private-user", "private-password", std::chrono::hours(24) + std::chrono::minutes(16)};
    expect(viewer::online_credential_notice({}, paired, now).empty() && viewer::online_credential_notice(turn, paired, now).empty(),
        "No relay warning without credentials or before the last 15 minutes");
    const auto first_credential_warning = viewer::online_credential_notice(turn, paired, now + std::chrono::minutes(1));
    const auto urgent_credential_warning = viewer::online_credential_notice(turn, paired, now + std::chrono::minutes(14));
    expect(first_credential_warning.find("15 min") != std::string::npos && first_credential_warning.find("EAWR-NET-1736") != std::string::npos,
        "Relay credential warning begins at 15 minutes independently of a renewed healthy lease");
    expect(urgent_credential_warning.find("2 min") != std::string::npos && urgent_credential_warning.find("about to") != std::string::npos,
        "Relay credential warning escalates at two minutes");
    expect(!viewer::online_relay_allocation_ended(viewer::LanReason::peer_left, turn, paired, now + std::chrono::minutes(16) - std::chrono::seconds(1))
        && viewer::online_relay_allocation_ended(viewer::LanReason::peer_left, turn, paired, now + std::chrono::minutes(16)), "1737 mapping begins exactly at the credential boundary");
    expect(!viewer::online_relay_allocation_ended(viewer::LanReason::cancelled, turn, paired, now + std::chrono::minutes(20))
        && !viewer::online_relay_allocation_ended(viewer::LanReason::peer_left, {}, paired, now + std::chrono::minutes(20)), "1737 requires peer loss and relay credentials");
    for (const auto& text : {warning, final_notice, first_credential_warning, urgent_credential_warning}) {
        expect(text.find("token") == std::string::npos && text.find("password") == std::string::npos && text.find("turn:") == std::string::npos && text.find("private-user") == std::string::npos,
            "notices carry no credentials or service addresses");
    }
}

int main() {
    try {
        long_session_contracts();
        viewer::LanView measured;
        measured.connected = true; measured.remote_endpoint = "127.0.0.1:47624";
        std::vector<eawr::presentation::ui::LobbyGame> games(2);
        games[0].address = measured.remote_endpoint; games[1].address = "127.0.0.2:47624";
        viewer::apply_lan_lobby_ping(games, measured);
        expect(eawr::presentation::ui::lobby_row_text(games[0]).ping == "?", "unknown transport ping remains unknown");
        measured.link_stats = eawr::net::LinkStats{std::chrono::microseconds(82000), std::chrono::microseconds(80500),
            std::chrono::microseconds(2000), 4};
        viewer::apply_lan_lobby_ping(games, measured);
        expect(eawr::presentation::ui::lobby_row_text(games[0]).ping == "80ms" &&
            eawr::presentation::ui::lobby_row_text(games[1]).ping == "?", "only linked endpoint displays measured ping");
        measured.link_stats->smoothed_rtt = std::chrono::microseconds(50);
        expect(viewer::lan_lobby_ping(measured.link_stats) == 1, "submillisecond measured LAN RTT stays known");
        measured.connected = false; games[0].ping_ms = 0;
        viewer::apply_lan_lobby_ping(games, measured);
        expect(games[0].ping_ms == 0, "disconnected snapshot does not apply stale ping");
        eawr::net::MatchSpeedWindow window;
        expect(window.accept(100) && window.accept(200) && !window.accept(300), "MP-U1 two requests per local window");
        expect(!window.accept(45'100) && window.accept(45'101), "oldest expires strictly after 45 seconds");
        expect(!window.accept(45'200) && window.accept(45'201), "expiry retains the younger accepted request");
        for (std::uint32_t tactical_speed = 0; tactical_speed < 5; ++tactical_speed)
            for (std::uint32_t strategic_speed = 0; strategic_speed < 5; ++strategic_speed) {
                const eawr::net::MatchOptions options{tactical_speed, strategic_speed};
                const auto encoded = require(eawr::net::encode_match_options(options));
                expect(encoded.size() == eawr::net::match_options_domain.size() + 8, "MP-06 two 32-bit choices");
                expect(require(eawr::net::decode_match_options(encoded)) == options, "all mode choices round-trip");
                auto malformed = encoded; malformed.push_back(0);
                expect(!eawr::net::decode_match_options(malformed), "trailing options bytes refused");
            }
        expect(!eawr::net::encode_match_options({5, 2}) && !eawr::net::encode_match_options({2, 5}), "both options ranges checked");
        expect(!make_match()->request_speed(4), "speed requests refused outside running match");
        viewer::LanView recovery_copy; recovery_copy.phase = viewer::LanPhase::setup;
        expect(!viewer::lan_reconnect_disabled_notice(recovery_copy), "production lobby has no disabled-reconnect line without a provider");
        recovery_copy.credential_provider_available = true;
        expect(viewer::lan_reconnect_disabled_notice(recovery_copy), "wired but disabled policy explains reconnect in the lobby");
        recovery_copy.reconnect_enabled = true;
        expect(!viewer::lan_reconnect_disabled_notice(recovery_copy), "enabled reconnect has no disabled notice");
        for (const auto* state : {"host_waiting", "joined", "review", "refused", "waiting", "desync", "result", "loss"}) {
            const auto fake = viewer::make_fake_lan_match(state);
            expect(!viewer::lan_message(fake->view()).empty(), "fake state has plain-language presentation");
        }
        auto invalid = make_match(); invalid->join("invalid:0");
        expect(invalid->view().phase == viewer::LanPhase::ended, "invalid endpoint is refused without a socket");
        expect(invalid->view().message.find("EAWR-NET-0211") != std::string::npos &&
            invalid->view().message.find("hostname") != std::string::npos, "invalid endpoint has clear corrective error text");
        recovery_presentation_contract();
        lan_ai_defaults_follow_authored_constraints(); lobby_refusal_and_placeholder_contracts(); shared_builder_matches_former_live_encoding(); terminal_lobby_commands_are_inert();
        close_one_mesh_edge(); authoritative_staging(false); authoritative_staging(true);
        authoritative_staging(true, net::LobbyReason::kicked); authoritative_staging(true, net::LobbyReason::host_closed);
        missing_planned_owner_is_refused();
        nine_seat_staging({1, 1, 1, 2, 2, 2, 3, 3, 3});
        nine_seat_staging({1, 1, 2});
        nine_seat_staging({1, 1, 1, 1, 1, 1, 1, 1, 1});
        staging_ingress_bounds();
        player_row_parser(); refused_rosters();
        for (const auto count : {3U, 4U, 8U, 9U}) { shared_mesh(count, false); shared_mesh(count, true); }
        canonical_join_ping_key(); transport_failure_copy(); connect_failure_copy();
        threaded_loading_pair();
        expect(viewer::live_begin_barrier(false) && !viewer::live_begin_barrier(true), "SP Begin, LAN readiness");
        expect(!viewer::lan_can_abort(viewer::make_fake_lan_match("waiting")->view()), "running quit uses agreement");
        for (const auto* state : {"result", "loss"})
            expect(!viewer::lan_escape_aborts(viewer::make_fake_lan_match(state)->view()),
                "Escape on either decided result follows the end-panel input route");
        for (const auto* state : {"barrier", "peer_left", "failed_peer", "reconnecting", "resyncing"})
            expect(viewer::lan_escape_aborts(viewer::make_fake_lan_match(state)->view()),
                "pending and failed pre-tick-zero battles retain Escape exit");
        for (int side : {0, 1}) {
            threaded_loading_pair(side, true);
            threaded_loading_pair(side);
            for (unsigned race = 0; race < 4; ++race) threaded_loading_pair(side, false, true);
        }
        for (const auto workers : {1U, 2U, 4U, 8U})
            for (const auto recovery : {5, 6, 7}) loopback(false, false, false, recovery, false, {}, {}, workers);
        for (const auto disagreement : {1, 2, 3, 4, 5, 6}) loopback(false, false, false, 6, false, {}, {}, 1, false, disagreement);
        for (const auto recovery : {8, 9}) loopback(false, false, false, recovery);
        for (const auto workers : {1U, 2U, 4U, 8U}) loopback(false, false, false, 5, false, {}, {}, workers, true);
        loopback(false, false, true);
        loopback(false, false, false, 2);
        loopback(false, false, false, 1);
        loopback(false, false, false, 4);
        loopback(false);
        loopback(true);
        loopback(true, false, false, 0, true);
        loopback(true, true);
        loopback(false, false, false, 3); // last: terminator refuses any later worker launch
        viewer::shutdown_lan_drains();
        std::cout << "viewer LAN facade contracts passed\n";
    } catch (const std::exception& error) { viewer::shutdown_lan_drains(); std::cerr << stage << ": " << error.what() << '\n'; return 1; }
    return 0;
}
