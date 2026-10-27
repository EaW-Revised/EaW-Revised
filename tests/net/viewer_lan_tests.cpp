#include "viewer_lan_fixtures.hpp"
#include "lan_match.hpp"
#include "lan_match_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "lan_diagnostics.hpp"
#include "../../src/net/platform_socket.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/lan_credentials.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/platform/live_session.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "fake_transport.hpp"
#include "eawr/net/lobby_session.hpp"
#include "eawr/net/match_options.hpp"
#include "eawr/net/transport.hpp"
#include "eawr/presentation/ui/lan_lobby.hpp"
#include "eawr/net/gns_signalling.hpp"
#include "eawr/net/gns_transport.hpp"
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/mesh_incident.hpp"
#include "eawr/net/mesh_policy.hpp"
#include "eawr/net/port_mapping.hpp"
#include "eawr/net/reconnect.hpp"
#include "eawr/net/setup.hpp"
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace viewer = eawr::presentation::godot_backend;
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;

namespace viewer_lan_test {
std::string stage;
std::shared_ptr<viewer::LanMatch> make_match(std::filesystem::path root, std::uint64_t delay,
    bool progress, std::filesystem::path trace) {
    auto match = viewer::make_lan_match(std::move(root), delay, progress, std::move(trace));
    match->configure_factions({"Rebel", "Empire", "Underworld", "Mod_Fleet_C"});
    return match;
}

// A human quitting while the match is held: the hold survives while another human stays, and clears with the last one.
void pause_departure_contracts() {
    stage = "multiplayer pause with departing humans";
    using Adapter = viewer::lan_detail::LiveAdapter;
    const auto initial = human_setup(2);
    const std::vector<net::SetupSeat> roster{{0, {1}, 1}, {1, {2}, 2}};
    Adapter adapter(initial, roster, nullptr);
    const auto pause_input = [](std::uint32_t seat, std::uint64_t sequence, bool paused) {
        return net::SeatInput{{0, {seat}, sequence}, require(net::encode_match_pause(paused))};
    };
    const auto quit_input = [](std::uint32_t seat, std::uint64_t sequence) {
        return net::SeatInput{{0, {seat}, sequence},
            require(net::encode_tactical_input({0, {seat}, sequence}, {{seat}, seat, true, false}, {{}, tactical::QuitPayload{}}))};
    };
    expect(static_cast<bool>(adapter.install(std::vector{pause_input(1, 0, true)})) && adapter.clock.paused, "seat 1 holds the match");
    expect(static_cast<bool>(adapter.install(std::vector{quit_input(2, 1)})), "the other human quits while held");
    expect(adapter.clock.paused, "one human still present: the hold stays and can be resumed by that human");
    expect(static_cast<bool>(adapter.install(std::vector{quit_input(1, 2)})), "the last human quits while held");
    expect(!adapter.clock.paused, "no human left: the hold is cleared so AI fallback can run");
}

void multiplayer_pause_contracts() {
    stage = "multiplayer pause, journal and liveness";
    pause_departure_contracts();
    using Adapter = viewer::lan_detail::LiveAdapter;
    using Change = net::MatchPauseClock::Change;
    net::MatchPauseClock limiter;
    expect(limiter.apply(true, {10, {1}, 0}) == Change::applied, "first pause admitted");
    expect(limiter.apply(true, {10, {2}, 0}) == Change::unchanged && limiter.actor == sim::SeatId{1}, "concurrent pause is a no-op and preserves actor");
    expect(limiter.apply(false, {11, {2}, 1}) == Change::applied, "another human can resume");
    expect(limiter.apply(true, {50, {1}, 1}) == Change::applied, "a spaced second pause admitted");
    expect(limiter.apply(false, {51, {1}, 2}) == Change::applied, "resume is never refused");
    expect(limiter.apply(true, {60, {1}, 3}) == Change::throttled, "a pause right after this seat's resume is debounced");
    expect(limiter.apply(true, {81, {1}, 4}) == Change::applied, "the debounce is one session second");
    for (std::uint64_t round = 0; round < 20; ++round) {
        expect(limiter.apply(false, {100 + round * 40, {2}, 10 + round * 2}) == Change::applied, "repeated resume is never refused");
        expect(limiter.apply(true, {120 + round * 40, {1}, 11 + round * 2}) == Change::applied, "repeated spaced pauses are never refused");
    }
    auto malformed = require(net::encode_match_pause(true)); malformed.push_back(0);
    expect(!net::decode_match_pause(malformed), "control refuses trailing bytes");
    expect(!net::parse_match_pause_replay({}), "older or missing session journal refused by named version");
    for (const unsigned count : {2U, 3U}) for (const unsigned workers : {1U, 2U, 4U, 8U}) {
        const auto initial = human_setup(count);
        const auto tables = content();
        eawr::platform::ThreadWorkerAdapter executor(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
        eawr::testing::FakeNetwork network(count);
        std::vector<net::SetupSeat> roster;
        std::vector<net::PeerSeat> seats;
        for (unsigned peer = 0; peer < count; ++peer) {
            roster.push_back({peer, {peer + 1}, peer + 1}); seats.push_back({{peer + 1}, true});
        }
        std::vector<tactical::TacticalSession> worlds;
        worlds.reserve(count);
        std::vector<eawr::platform::LiveStepContext> contexts(count);
        std::vector<std::unique_ptr<Adapter>> adapters;
        std::vector<std::unique_ptr<eawr::testing::FakeTransport>> transports;
        std::vector<net::LockstepSession> sessions;
        sessions.reserve(count);
        std::vector<std::vector<std::string>> hashes(count);
        for (unsigned peer = 0; peer < count; ++peer) worlds.push_back(require(tactical::TacticalSession::create(initial, tables)));
        const auto bind = [&](eawr::platform::LiveStepContext& context, tactical::TacticalSession& world) {
            auto* target = &world;
            context.completed_tick = [target] { return target->completed_tick(); };
            context.state_hash = [target] { return target->state_sha256(); };
            context.world_hash = context.state_hash;
            context.canonical_state = [target] { return target->canonical_state_bytes(); };
            context.snapshot = [target] { return target->snapshot(); };
            context.install = [target](std::span<const tactical::PlayerCommand> commands) {
                for (const auto& command : commands) { auto accepted = target->submit(command); if (!accepted) return accepted; }
                return eawr::core::Result<void>::success();
            };
            context.step = [target, &executor] {
                auto stepped = target->step(executor);
                return stepped ? eawr::core::Result<void>::success() : eawr::core::Result<void>::failure(stepped.error());
            };
        };
        for (unsigned peer = 0; peer < count; ++peer) {
            bind(contexts[peer], worlds[peer]);
            adapters.push_back(std::make_unique<Adapter>(initial, roster, nullptr)); adapters.back()->context = &contexts[peer];
            adapters.back()->recording.setup = require(net::encode_skirmish_setup(initial));
            transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, peer));
            net::SessionOptions options; options.final_tick_count = 25'000;
            sessions.push_back(require(net::LockstepSession::create(*adapters.back(), *transports.back(), seats, options)));
        }
        std::uint64_t now = 0;
        const auto advance = [&](std::uint64_t boundary) {
            for (unsigned attempt = 0; attempt < 100'000; ++attempt) {
                bool complete = true;
                now += 34; network.round = now;
                for (unsigned peer = 0; peer < count; ++peer) {
                    if (adapters[peer]->completed_tick() >= boundary) continue;
                    complete = false;
                    auto result = require(sessions[peer].pump(now));
                    if (result.completed) hashes[peer].push_back(result.completed->state_sha256);
                }
                if (complete) return;
            }
            expect(false, "pause fixture reached its agreed boundary");
        };
        advance(10);
        const auto pause = require(sessions[0].submit([](sim::SeatCommandKey) { return net::encode_match_pause(true); }));
        // Two seats racing to pause must still need only one Resume.
        require(sessions[1].submit([](sim::SeatCommandKey) { return net::encode_match_pause(true); }));
        advance(pause.tick + 1);
        const auto held_tick = worlds[0].completed_tick();
        const auto held_state = worlds[0].canonical_state_bytes();
        for (unsigned peer = 0; peer < count; ++peer)
            expect(adapters[peer]->clock.paused && worlds[peer].completed_tick() == held_tick, "peers pause on the same world boundary");
        const auto unit = std::find_if(initial.units.begin(), initial.units.end(), [](const auto& row) { return row.owner == 1; });
        expect(unit != initial.units.end(), "fixture has a controllable unit");
        require(sessions[0].submit([&](sim::SeatCommandKey key) {
            return net::encode_tactical_input(key, {{1}, 1, true, false}, {{unit->entity_id}, tactical::MovePayload{{units(100), units(100), {}}}});
        }));
        advance(pause.tick + 18'001); // ten nominal minutes of healthy control-only session steps
        for (unsigned peer = 0; peer < count; ++peer) {
            expect(worlds[peer].completed_tick() == held_tick && worlds[peer].canonical_state_bytes() == held_state,
                "ten-minute pause freezes all tactical world state and timers, including queued orders");
            expect(!sessions[peer].waiting_for(), "healthy paused peers are not stalled");
            expect(hashes[peer] == hashes[0], "pause produces equal per-session digests across peers");
        }
        {
            auto restored = require(tactical::TacticalSession::create(initial, tables));
            eawr::platform::LiveStepContext restored_context; bind(restored_context, restored);
            Adapter staged(initial, roster, nullptr); staged.context = &restored_context;
            const auto paused_journal = require(net::parse_match_pause_replay(require(adapters[0]->record_controls())));
            require(net::replay_match_pause(paused_journal, staged, [&] { return restored.completed_tick(); }));
            expect(staged.clock.paused && staged.state_hash() == adapters[0]->state_hash(),
                "a paused recovery prefix restores the hold, actor, limiter and deferred orders");
        }
        const auto resume = require(sessions[1].submit([](sim::SeatCommandKey) { return net::encode_match_pause(false); }));
        advance(resume.tick + 5);
        for (unsigned peer = 0; peer < count; ++peer)
            expect(!adapters[peer]->clock.paused && worlds[peer].completed_tick() > held_tick && hashes[peer] == hashes[0],
                "another active human resumes once with equal world and session digests");
        const auto encoded = require(adapters[0]->record_controls());
        const auto journal = require(net::parse_match_pause_replay(encoded));
        auto replay_world = require(tactical::TacticalSession::create(initial, tables));
        eawr::platform::LiveStepContext replay_context; bind(replay_context, replay_world);
        Adapter replay_adapter(initial, roster, nullptr); replay_adapter.context = &replay_context;
        require(net::replay_match_pause(journal, replay_adapter, [&] { return replay_world.completed_tick(); }));
        expect(replay_world.canonical_state_bytes() == worlds[0].canonical_state_bytes()
            && replay_adapter.state_hash() == adapters[0]->state_hash(), "outer journal reconstructs control and world state without wall waits");
        {
            auto playback = std::make_shared<viewer::lan_detail::LivePauseReplay>(initial, roster, journal);
            eawr::platform::LiveSession::Options playback_options;
            playback_options.workers = workers;
            playback_options.pacing = eawr::platform::LiveSession::Pacing::driven;
            playback_options.recorded_step_driver = [playback](eawr::platform::LiveStepContext& context,
                std::span<const tactical::PlayerCommand> recorded, std::uint64_t) { return playback->step(context, recorded); };
            const auto world_recording = worlds[0].record();
            auto live = require(eawr::platform::LiveSession::start(world_recording, tables, playback_options));
            live->advance_to(world_recording.final_tick_count);
            expect(live->wait_for(world_recording.final_tick_count, std::chrono::seconds(30)), "normal replay source consumes the session journal without wall waits");
            live->stop();
            expect(!live->failure() && playback->complete() && live->record() == world_recording,
                "viewer playback reconstructs every control boundary using the unchanged world recording");
        }
        auto truncated = encoded; truncated.pop_back();
        expect(!net::parse_match_pause_replay(truncated), "truncated journal refused");
        // Physical loss still terminates a paused session; no pause exemption hides it.
        require(sessions[0].submit([](sim::SeatCommandKey) { return net::encode_match_pause(true); }));
        advance(adapters[0]->completed_tick() + 10);
        network.open[count - 1] = false;
        bool lost = false;
        for (unsigned attempt = 0; attempt < 100 && !lost; ++attempt) {
            now += 34; network.round = now;
            const auto result = sessions[0].pump(now);
            if (!result) lost = result.error().code == net::session_errors::link_closed;
        }
        expect(lost && adapters[0]->clock.paused, "real drop while paused is still detected");
    }
}

void diagnostic_contract() {
    stage = "bounded normal-log diagnostics";
    viewer::LanDiagnostics log;
    std::vector<std::string> lines;
    log.sink = [&](std::string_view line) { lines.emplace_back(line); };
    log.seat = 0; log.tick = 7314;
    const std::array<net::PeerIndex, 1> peers{1};
    log.stall(true, 0, peers); log.stall(true, 999, peers);
    expect(lines.empty(), "subsecond lockstep gaps do not spam the log");
    log.stall(true, 1000, peers);
    for (std::uint64_t now = 1001; now < 1100; ++now) log.stall(true, now, peers);
    log.stall(false, 1100); log.stall(false, 1999);
    expect(lines.size() == 1 && lines[0].find("event=stall-start") != std::string::npos
        && lines[0].find("peer=1 tick=7314") != std::string::npos, "sustained stall identifies missing peer and tick exactly once");
    log.tick = 7315; log.stall(false, 2000);
    expect(lines.size() == 2 && lines[1].find("event=stall-end") != std::string::npos
        && lines[1].find("duration_ms=1100") != std::string::npos, "quick resume is logged at the one-second rate boundary with actual duration");
    for (std::uint64_t now = 2100; now < 2200; ++now) { log.stall(true, now, peers); log.stall(false, now); }
    expect(lines.size() == 2, "ordinary input gaps do not create per-tick traffic");
    viewer::LanView view; view.phase = viewer::LanPhase::reconnecting;
    view.recovery = viewer::LanRecovery::held; view.reconnect_seat = 1; view.recovery_epoch = 9;
    log.view(view); log.view(view);
    expect(lines.size() == 3 && lines.back().find("reason=held epoch=9") != std::string::npos, "held-seat incident logs once per transition");
    view.phase = viewer::LanPhase::ended; view.reason = viewer::LanReason::timeout;
    view.recovery = viewer::LanRecovery::rejoin_failed; view.diagnostic_code = "EAWR-NET-1005";
    log.view(view); const auto count = lines.size(); log.view(view);
    expect(lines.size() == count && lines.back().find("event=battle-end") != std::string::npos
        && lines.back().find("code=EAWR-NET-1005") != std::string::npos, "terminal diagnostics retain reason code without repeated frame logs");
    struct Transport final : net::MessageTransport {
        net::LinkState state{net::LinkState::open};
        eawr::core::Result<void> send(net::PeerIndex, std::span<const std::uint8_t>) override { return eawr::core::Result<void>::success(); }
        std::optional<net::Received> poll() override { return {}; }
        net::LinkState link(net::PeerIndex) const override { return state; }
        net::PeerIndex self() const override { return 0; }
        std::uint32_t peer_count() const override { return 2; }
    } transport;
    const auto before_links = lines.size();
    const auto reason = [](net::PeerIndex) { return std::string("EAWR-NET-1769"); };
    log.links(transport, reason); log.links(transport, reason);
    transport.state = net::LinkState::closed; log.links(transport, reason); log.links(transport, reason);
    expect(lines.size() == before_links + 3 && lines[before_links + 1].find("event=link-lost") != std::string::npos
        && lines.back().find("reason=EAWR-NET-1769 state=closed") != std::string::npos,
        "pair-specific closure code survives transition deduplication without a fatal whole-room error");
    viewer::LanView loss; loss.phase = viewer::LanPhase::ended; loss.reason = viewer::LanReason::timeout;
    loss.diagnostic_code = "EAWR-NET-0106"; loss.message = "Connection lost: the match could not continue.\nNamed opponent\nEAWR-NET-0106";
    expect(viewer::lan_message(loss) == loss.message && !loss.victory,
        "terminal timeout keeps named connection-loss text instead of generic timeout or victory copy");
    for (const auto code : {net::mesh_errors::no_majority, net::mesh_errors::no_decision, net::mesh_errors::overlap}) {
        expect(!viewer::lan_connection_loss(code), "mesh decisions retain their own curated failure text");
    }
    log.sink = [](std::string_view) { throw std::runtime_error("unavailable log sink"); };
    log.emit("test", "none", 1);
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
        auto join = make_match();
        std::vector<std::string> diagnostics;
        join->set_diagnostic_logger([&](std::string_view line) { diagnostics.emplace_back(line); });
        join->join("127.0.0.1:" + std::to_string(host->port()));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (join->view().phase != viewer::LanPhase::ended) {
            expect(std::chrono::steady_clock::now() < deadline, "failure copy handshake deadline");
            (void)host->poll(); join->pump_setup({}); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(viewer::lan_message(join->view()) == (version ? "The other player uses a different network version." :
            "The other player uses a different game build."), "existing mismatch copy remains curated");
        expect(std::any_of(diagnostics.begin(), diagnostics.end(), [&](const auto& line) {
            return line.find(version ? "reason=EAWR-NET-0201" : "reason=EAWR-NET-0202") != std::string::npos;
        }), "real transport refusal reaches the caller logger without losing its reason code");
        expect(std::any_of(diagnostics.begin(), diagnostics.end(), [](const auto& line) {
            return line.find("event=battle-end") != std::string::npos;
        }), "setup failure has a normal-log terminal reason");
        // The collector must outlive cancellation and any terminal transport drain.
        join->set_diagnostic_logger({});
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

// Ticket E: long sessions. Synthetic tick counts only; nothing here waits for real hours.
static void long_session_contracts() {
    {
        viewer::LanView online;
        online.phase = viewer::LanPhase::running;
        viewer::LanDepartureNotices notices;
        notices.push("Rex left: their connection to Echo failed.", 0);
        online.mesh_notice = notices.text(0);
        const auto incident = viewer::lan_incident_message(online);
        expect(incident == online.mesh_notice, "the battle shows the confirmed named pair-loss notice exactly once");
        expect(notices.text(viewer::LanDepartureNotices::display_ms - 1) == incident, "the departure notice remains readable briefly");
        online.mesh_notice = notices.text(viewer::LanDepartureNotices::display_ms);
        expect(viewer::lan_incident_message(online).empty(), "a later frame has no expired departure banner");
        expect(notices.text(viewer::LanDepartureNotices::display_ms + 1).empty(), "the incident history cannot republish an expired notice");
        notices.push("Quinn left: their connection to Echo failed.", 20000);
        expect(notices.text(20000).find("Quinn") != std::string::npos && notices.text(20000).find("Rex") == std::string::npos,
            "a later departure shows only its new notice");
        online.phase = viewer::LanPhase::ended;
        online.recovery = viewer::LanRecovery::rejoin_failed;
        online.diagnostic_code = "EAWR-NET-1778";
        online.message = "Connection to Echo could not be restored. You have left the match.";
        const auto ended = viewer::lan_message(online);
        expect(ended.starts_with(online.message), "a final named mesh decision is not hidden by generic recovery text");
        const auto code_at = ended.find(online.diagnostic_code);
        expect(code_at != std::string::npos && ended.find(online.diagnostic_code, code_at + 1) == std::string::npos,
            "the final mesh code is shown once");
        net::GnsPairDiagnostic pair;
        pair.phase = net::GnsPairPhase::signalling;
        expect(viewer::online_pair_message(pair) == "Connecting", "a forming pair is not called connected");
        pair.phase = net::GnsPairPhase::open; pair.route = net::GnsRoute::relayed;
        expect(viewer::online_pair_message(pair) == "Connected through relay", "an established relay is explained");
        pair.phase = net::GnsPairPhase::failed; pair.relay_offered = true;
        expect(viewer::online_pair_message(pair).find("relay was offered") != std::string::npos, "a failed pair says whether relay was offered");
        pair.relay_offered = false;
        expect(viewer::online_pair_message(pair).find("no relay was available") != std::string::npos, "an unoffered relay is never claimed attempted");
        const std::array repair_phases{
            std::pair{net::GnsPairPhase::refreshing, "Refreshing room credentials"},
            std::pair{net::GnsPairPhase::repairing, "Restoring connection"},
            std::pair{net::GnsPairPhase::authenticating, "Checking restored connection"}};
        for (const auto& [phase, expected] : repair_phases) {
            pair.phase = phase;
            expect(viewer::online_pair_message(pair) == expected, "each repair phase has its exact curated text");
        }
        const std::array repair_causes{"Connection repair is unavailable", "Room credentials could not be refreshed",
            "Restored connection could not be authenticated", "Connection repair timed out", "Retained game data could not be restored"};
        const std::array terminal_causes{"Connection repair is unavailable. You have left the match.",
            "The online service could not restore your connection. You have left the match.",
            "The returning connection could not be authenticated. You have left the match.",
            "The connection could not be restored in time. You have left the match.",
            "The connection could not catch up within the recovery limit. You have left the match."};
        for (unsigned code = 1790; code <= 1794; ++code) {
            pair.phase = net::GnsPairPhase::lost; pair.code = code;
            expect(viewer::online_pair_message(pair) == repair_causes[code - 1790], "each pair repair failure has its exact curated cause");
            expect(viewer::online_mesh_failure_text("EAWR-NET-" + std::to_string(code)) == terminal_causes[code - 1790],
                "each terminal repair failure has its exact cause, including unavailable 1790");
        }
        eawr::testing::FakeNetwork failed_delivery(3);
        failed_delivery.fail_send = [](auto, auto, auto) { return true; };
        eawr::testing::FakeTransport delivery_stream(failed_delivery, 0);
        auto repair_offer = offer(); repair_offer.recovery.reconnect = false;
        repair_offer.setup = require(net::encode_skirmish_setup(human_setup(3)));
        repair_offer.seats.push_back({2, sim::SeatId{12}, 3});
        net::LanReconnectCredentials credentials;
        const auto delivery = credentials.provide_mesh_repair(repair_offer, delivery_stream);
        expect(!delivery && delivery.error().code == "EAWR-NET-1022", "a real admitted-stream send failure reports transient credential delivery");
        bool room_open = true;
        std::vector<std::string> start_actions;
        const auto feedback = [&](const eawr::core::Diagnostic& error) { start_actions.push_back("feedback:" + error.code); };
        const auto end_room = [&](const eawr::core::Diagnostic& error) { room_open = false; start_actions.push_back("end:" + error.code); };
        viewer::handle_online_start_failure(delivery.error(), feedback, end_room);
        expect(room_open && start_actions == std::vector<std::string>{"feedback:EAWR-NET-1022"},
            "transient Start delivery failure keeps the host room open and reaches lobby feedback");
        start_actions.clear();
        eawr::core::Diagnostic entropy_failure; entropy_failure.code = "EAWR-NET-1021";
        viewer::handle_online_start_failure(entropy_failure, feedback, end_room);
        expect(!room_open && start_actions == std::vector<std::string>{"feedback:EAWR-NET-1021", "end:EAWR-NET-1021"},
            "OS entropy failure reaches feedback before terminal cleanup");
        online.phase = viewer::LanPhase::reconnecting; online.recovery = viewer::LanRecovery::held;
        online.online_capacity = 9; online.reconnect_enabled = false; online.diagnostic_code.clear();
        online.message = "Connection to Echo: Checking restored connection. Play is paused.";
        const auto repairing = viewer::lan_message(online);
        expect(repairing == online.message && repairing.find("Incident cap") == std::string::npos,
            "the named mesh hold keeps its message without unrelated returning-seat policy timers");
        expect(!viewer::lan_gameplay_enabled(online) && viewer::lan_recovery_audio_suppressed(online), "a mesh hold gates inputs and audio");
        expect(viewer::online_refusal_text({}, {}, 8, true) != viewer::online_refusal_text({}, {}, 8, false),
            "newcomer refusals preserve whether relay was offered at nine seats");
        net::LobbyChat posted; posted.text = "Rex could not connect to Echo and was not admitted.";
        net::LobbyNotice notice; notice.reason = net::LobbyReason::pair_unreachable;
        notice.message = posted.text + " " + net::lobby_diagnostic(notice.reason, "").code;
        expect(viewer::lan_notice_in_chat(notice, {posted}), "the host's pair refusal already in chat is not shown twice");
        expect(!viewer::lan_notice_in_chat(notice, {}), "a notice without its broadcast remains visible");
        notice.reason = net::LobbyReason::kicked;
        expect(!viewer::lan_notice_in_chat(notice, {posted}), "ordinary removal notices are not suppressed");
    }
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
    // A refused newcomer's own text names the player it could not reach (owner decision 1): the host's closed notice cannot
    // reach a seat whose transport is already fatal, so the newcomer composes it from the roster it saw.
    {
        std::vector<net::LobbySeat> seats(3);
        seats[0].peer = 0; seats[0].selection.name = "Host";
        seats[1].peer = 2; seats[1].selection.name = "Rex";
        expect(viewer::online_refusal_name(seats, 2) == "Rex", "the unreachable player is named from the newcomer's roster");
        expect(viewer::online_refusal_name(seats, 5) == "another player in this room", "an unknown slot falls back to the generic name");
        expect(net::mesh_newcomer_refusal_text(viewer::online_refusal_name(seats, 2), true).find("Rex") != std::string::npos,
            "the refusal text carries the name");
        // A refusal that overtakes the admission snapshot: the newcomer holds no roster, the host stream still queues one.
        net::LobbySnapshot snapshot; snapshot.room = "room"; snapshot.capacity = 4;
        snapshot.seats.resize(2);
        snapshot.seats[0].peer = 0; snapshot.seats[0].seat = sim::SeatId{1}; snapshot.seats[0].player = 1; snapshot.seats[0].selection.name = "Host";
        snapshot.seats[1].peer = 2; snapshot.seats[1].seat = sim::SeatId{2}; snapshot.seats[1].player = 2; snapshot.seats[1].selection.name = "Rex";
        const auto snapshot_frame = net::encode_lobby_frame(snapshot);
        expect(snapshot_frame.has_value(), "the snapshot frame encodes");
        const std::vector<net::Received> queued_snapshot{{0, snapshot_frame.value()}};
        expect(viewer::online_refusal_text(queued_snapshot, {}, 2, true).find("Rex") != std::string::npos,
            "a newcomer without a roster still names the player from the queued admission snapshot");
        expect(viewer::online_refusal_text({}, {}, 2, true).find("another player in this room") != std::string::npos,
            "nothing queued and no roster: the generic name");
        expect(viewer::online_refusal_text({}, seats, 2, true).find("Rex") != std::string::npos, "a roster seen earlier names the player");
        // The frame the host really sends: LobbySession::refuse_unreachable removes the newcomer with a LobbyClosed whose text is
        // mesh_newcomer_refusal_text (lobby_session_tests pins that); the newcomer shows the host's words verbatim.
        net::LobbyClosed closed; closed.room = "room"; closed.revision = 2; closed.reason = net::LobbyReason::pair_unreachable;
        closed.message = net::mesh_newcomer_refusal_text("Quinn", true);
        const auto closed_frame = net::encode_lobby_frame(closed);
        expect(closed_frame.has_value(), "the host's refusal frame encodes");
        const std::vector<net::Received> queued_notice{{0, snapshot_frame.value()}, {0, closed_frame.value()}};
        const auto from_host = viewer::online_refusal_text(queued_notice, {}, 2, true);
        expect(from_host == closed.message && from_host.find("Quinn") != std::string::npos && from_host.find("Rex") == std::string::npos,
            "the host's own closed notice wins over the roster");
        expect(viewer::online_refusal_text({{1, closed_frame.value()}}, {}, 2, true).find("another player in this room") != std::string::npos,
            "a notice that did not come from the host is not trusted");
        net::LobbyClosed other_reason = closed; other_reason.reason = net::LobbyReason::host_closed;
        expect(viewer::online_refusal_text({{0, net::encode_lobby_frame(other_reason).value()}}, {}, 2, true).find("Quinn") == std::string::npos,
            "only a pair-unreachable notice is shown as the refusal");
    }
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
}
using namespace viewer_lan_test;

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--pause") {
            multiplayer_pause_contracts(); viewer::shutdown_lan_drains(); return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--pending-inputs") {
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                separate_mesh(2, false, true, workers);
            }
            viewer::shutdown_lan_drains();
            std::cout << "pending input contracts passed on two peers and 1/2/4/8 workers\n";
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--transient-repair-loop") {
            for (int run = 0, count = std::atoi(argv[2]); run < count; ++run) loopback(false, false, false, 8);
            viewer::shutdown_lan_drains();
            std::cout << "transient-repair loop passed\n";
            return 0;
        }
        long_session_contracts();
        viewer::LanView measured;
        measured.connected = true; measured.remote_endpoint = "127.0.0.1:47624";
        std::vector<eawr::presentation::ui::LobbyGame> games(2);
        games[0].address = measured.remote_endpoint; games[1].address = "127.0.0.2:47624";
        viewer::apply_lan_lobby_ping(games, measured);
        expect(eawr::presentation::ui::lobby_row_text(games[0]).ping == "?", "unknown transport ping remains unknown");
        measured.link_stats = eawr::net::LinkStats{std::chrono::microseconds(82000), std::chrono::microseconds(80500),
            std::chrono::microseconds(2000), 4, 0.0};
        viewer::apply_lan_lobby_ping(games, measured);
        expect(eawr::presentation::ui::lobby_row_text(games[0]).ping == "80ms" &&
            eawr::presentation::ui::lobby_row_text(games[1]).ping == "?", "only linked endpoint displays measured ping");
        measured.link_stats->smoothed_rtt = std::chrono::microseconds(50);
        expect(viewer::lan_lobby_ping(measured.link_stats) == 1, "submillisecond measured LAN RTT stays known");
        measured.connected = false; games[0].ping_ms = 0;
        viewer::apply_lan_lobby_ping(games, measured);
        expect(games[0].ping_ms == 0, "disconnected snapshot does not apply stale ping");
        eawr::net::MatchSpeedWindow window;
        for (std::uint64_t request = 0; request < 6; ++request) expect(window.accept(3 + request * 3), "MP-U1 six requests per session window");
        expect(!window.accept(21), "the seventh request is refused");
        expect(!window.accept(1'353) && window.accept(1'354), "oldest expires strictly after 45 session seconds");
        expect(!window.accept(1'354) && window.accept(1'357), "expiry retains the younger accepted requests");
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
        lan_ai_defaults_follow_authored_constraints(); lan_overflow_human_keeps_its_seat(); lobby_refusal_and_placeholder_contracts(); shared_builder_matches_former_live_encoding(); terminal_lobby_commands_are_inert();
        close_one_mesh_edge(); editable_staging_team_rules(); advanced_options_three_peers(); ai_difficulty_two_peers(); seat_controls_three_peers(); seat_controls_three_peers(true); authoritative_staging(false); authoritative_staging(true);
        authoritative_staging(true, net::LobbyReason::kicked); authoritative_staging(true, net::LobbyReason::host_closed);
        capacity_does_not_create_players();
        nine_seat_staging({1, 2, 3, 4, 5, 6, 7, 8, 9});
        nine_seat_staging({1, 2});
        staging_ingress_bounds();
        multiplayer_pause_contracts();
        diagnostic_contract(); player_row_parser(); refused_rosters();
        for (const auto count : {3U, 4U, 9U}) separate_mesh(count, false);
        for (const auto workers : {1U, 2U, 4U, 8U}) separate_mesh(8, false, false, workers);
        separate_mesh(4, true);
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
        loopback(false, false, false, 2, false, {}, {}, 1, false, 0, true); // script-only divergence in the last window
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
