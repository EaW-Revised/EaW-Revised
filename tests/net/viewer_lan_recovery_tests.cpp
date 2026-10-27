#include "viewer_lan_fixtures.hpp"
#include "lan_match.hpp"
#include "../../src/net/platform_socket.hpp"
#include "eawr/core/build_identity.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/lan_credentials.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/setup.hpp"
#include "eawr/net/tactical_input.hpp"
#include "eawr/net/transport.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
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
namespace { std::map<int, std::string> recovery_worker_hashes; }
void loopback(bool abrupt, bool departed_ahead, bool closed_before_launch, int final_window,
    bool speed_during_departure, std::shared_ptr<viewer::LanMatch> existing_host,
    std::shared_ptr<viewer::LanMatch> existing_join, unsigned workers, bool fail_recovery,
    int policy_disagreement, bool script_only) {
        // Names every scenario in the timing log (#4291); output only, no assertion depends on it.
        struct ScenarioTimer {
            std::string name; std::chrono::steady_clock::time_point began{std::chrono::steady_clock::now()};
            ~ScenarioTimer() {
                std::cout << "SCENARIO-SECONDS " << name << ' ' << std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count() << '\n';
            }
        } scenario_timer{"loopback abrupt=" + std::to_string(abrupt) + " departed_ahead=" + std::to_string(departed_ahead)
            + " closed_before_launch=" + std::to_string(closed_before_launch) + " final_window=" + std::to_string(final_window)
            + " speed_during_departure=" + std::to_string(speed_during_departure) + " workers=" + std::to_string(workers)
            + " fail_recovery=" + std::to_string(fail_recovery) + " policy_disagreement=" + std::to_string(policy_disagreement)
            + " script_only=" + std::to_string(script_only)};
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
        std::array<bool, 2> observed_hold{};
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
        // Guards against a hung scenario, not performance: slow or throttled pool hosts missed 20 s (#4282).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
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
            // script_only: the world hash stays equal and only the full (script) hash diverges from tick 136,
            // so the last window is detected only if the finish row carries the full hash.
            if (script_only) context.world_hash = [&, peer] { return worlds[peer].state_sha256(); };
            context.state_hash = [&, peer] {
                if (final_window == 2 && peer == 1 && (script_only ? worlds[peer].completed_tick() >= 136 : worlds[peer].completed_tick() == 136))
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
                    expect(host->request_speed(1) && host->request_speed(2) && host->request_speed(3) && host->request_speed(4) && host->request_speed(1)
                        && !host->request_speed(2), "seventh local change in window is refused");
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
                if (const auto seen = (peer == 0 ? host : join)->view().recovery;
                    seen == viewer::LanRecovery::held || seen == viewer::LanRecovery::catching_up || seen == viewer::LanRecovery::proving)
                    observed_hold[peer] = true; // Poll timing decides whether a survivor ever sees the hold.
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
            // The rejoin retry gate runs on this virtual clock; step it faster while the seat is held so a
            // 1 s virtual retry does not cost ~1000 real sleeps.
            clock += final_window == 8 && dropped && host->view().phase == viewer::LanPhase::reconnecting ? 100 : 1;
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
                const bool survivor_saw_hold = observed_hold[match == host ? 0U : 1U];
                if (final_window == 8 && view.active_adapter_publications == 0) {
                    const auto text = viewer::lan_message(view);
                    if (view.phase != viewer::LanPhase::running || view.recovery != viewer::LanRecovery::none || view.desync_tick)
                        std::cerr << "transient survivor view case=8 workers=" << workers << " phase=" << static_cast<int>(view.phase)
                            << " recovery=" << static_cast<int>(view.recovery) << " desync_tick=" << (view.desync_tick ? static_cast<long long>(*view.desync_tick) : -1LL)
                            << " publications=" << view.active_adapter_publications << " message=[" << text << "]\n";
                    // Poll timing decides whether the survivor ever observed the hold; only then may it show the recovered notice.
                    const bool clean = view.recovery == viewer::LanRecovery::none && text.find("Game synchronized.") == std::string::npos;
                    expect(view.phase == viewer::LanPhase::running && (clean || (survivor_saw_hold && view.recovery == viewer::LanRecovery::recovered))
                        && viewer::lan_gameplay_enabled(view) && !view.desync_tick && text.find("Incident at tick") == std::string::npos,
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
                    expect(match->view().desync_tick == (script_only ? 137U : 136U) && !match->view().bundle_path.empty(),
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
                expect(host->request_speed(3), "UI queues speed just before the loss deadline");
                clock += 30'001;
                const auto result = drivers[0](contexts[0], {}, clock++);
                expect(!result && result.error().code == net::session_errors::stalled,
                    "expired missing input ends as connection loss even with an optional speed request");
                expect(host->view().phase == viewer::LanPhase::ended && !host->view().victory && !host->view().departure,
                    "speed admission cannot turn an expired wait into a departure win");
                expect(host->view().options.tactical_speed == 4 && rates[0] == rates[1],
                    "unexecuted speed request never changes local pacing");
                join->cancel();
                return;
            }
            join->cancel(); // Abort the real ordered TCP stream without a quit frame.
            while (host->view().phase != viewer::LanPhase::ended) {
                expect(std::chrono::steady_clock::now() < deadline, "disconnect result deadline");
                const auto result = drivers[0](contexts[0], {}, clock++);
                if (!result) expect(result.error().code == net::session_errors::stalled, "lone survivor receives typed connection loss");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            expect(!host->view().departure && !host->view().victory
                && viewer::lan_message(host->view()).starts_with("Connection lost: the match could not continue"),
                "unconfirmed two-peer close shows connection loss, never a fabricated opponent quit or win");
            if (departed_ahead) expect(worlds[0].completed_tick() <= 130,
                "maximum-lead loss never executes beyond the validated receive frontier");
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
