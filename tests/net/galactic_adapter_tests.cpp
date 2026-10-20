#include "../galactic/galactic_support.hpp"
#include "fake_transport.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/synthetic.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// #3769: the production galaxy adapter under the REAL LockstepSession (fake delayed transport, desync monitor):
// seat authentication, payload classification, install at the agreed key, stepping only on the common frontier,
// and equal per-tick digests against an unnetworked reference at 1/2/4/8 workers.
namespace {

using galactic_test::expect;
namespace net = eawr::net;
namespace galactic = eawr::sim::galactic;
namespace math = eawr::sim::math;
using eawr::campaign::GameSession;
using eawr::testing::FakeDelivery;
using eawr::testing::FakeNetwork;
using eawr::testing::FakeTransport;
using Bytes = std::vector<std::uint8_t>;

template <class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
void require(const eawr::core::Result<void>& result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}

galactic::GalacticSetup versus_setup() {
    eawr::campaign::SyntheticCampaign campaign;
    campaign.name = "net-galactic-versus";
    campaign.galaxy.seed = 31;
    campaign.galaxy.planets = 24;
    campaign.galaxy.players = 3;
    campaign.galaxy.fleets_per_player = 2;
    campaign.galaxy.members_per_fleet = 3;
    campaign.galaxy.debug_commands = true;
    campaign.fiscal_cycle_seconds = math::Fixed::from_decimal("2.5").value();
    return require(eawr::campaign::build_synthetic_campaign(campaign));
}
// Versus with the recorded seat-control policy (WGS-P01..P04): a leaving human is replaced by its AI fallback seat.
galactic::GalacticSetup policy_setup() {
    auto setup = versus_setup();
    setup.seats.push_back({{4}, 1, eawr::sim::SeatKind::ai, 0, false, 0});
    setup.seats.push_back({{5}, 2, eawr::sim::SeatKind::ai, 0, false, 0});
    setup.seat_control.emplace();
    setup.seat_control->teams.push_back({1, {1}, false});
    setup.seat_control->teams.push_back({2, {2}, false});
    for (std::uint32_t player = 1; player <= 3; ++player)
        setup.seat_control->controllers.push_back({player,
            player <= 2 ? galactic::FactionControl::human : galactic::FactionControl::ai, galactic::FactionControl::ai, {}, {}});
    return setup;
}
galactic::GalacticSetup coop_policy_setup(const std::uint32_t humans) {
    auto setup = policy_setup();
    setup.seats.clear();
    for (std::uint32_t id = 1; id <= humans; ++id)
        setup.seats.push_back({{id}, 1, eawr::sim::SeatKind::human, id - 1, true, 1});
    for (std::uint32_t player = 1; player <= 3; ++player)
        setup.seats.push_back({{humans + player}, player, eawr::sim::SeatKind::ai, 0, player != 1, 0});
    setup.seat_control->teams = {{1, {1}, false}};
    setup.seat_control->controllers[1].kind = galactic::FactionControl::ai;
    setup.seat_control->controllers[0].configuration = {7, 11};
    setup.seat_control->controllers[0].state = {13, 17};
    return setup;
}
// Four humans on one side (seats 1, 4, 5, 6) against two AI seats.
galactic::GalacticSetup coop_setup() {
    eawr::campaign::SyntheticCampaign campaign;
    campaign.name = "net-galactic-coop";
    campaign.galaxy.seed = 37;
    campaign.galaxy.planets = 24;
    campaign.galaxy.players = 3;
    campaign.galaxy.fleets_per_player = 2;
    campaign.galaxy.members_per_fleet = 3;
    campaign.galaxy.coop_seat = true;
    campaign.galaxy.debug_commands = true;
    auto setup = require(eawr::campaign::build_synthetic_campaign(campaign));
    for (std::uint32_t id = 5; id <= 6; ++id) setup.seats.push_back({{id}, 1, eawr::sim::SeatKind::human, id - 1, true, 1});
    return setup;
}

std::vector<eawr::sim::SeatId> human_seats(const galactic::GalacticSetup& setup) {
    std::vector<eawr::sim::SeatId> seats;
    for (const auto& seat : setup.seats) if (seat.kind == eawr::sim::SeatKind::human) seats.push_back(seat.seat_id);
    std::sort(seats.begin(), seats.end());
    return seats;
}
galactic::PlayerId player_of(const galactic::GalacticSetup& setup, const eawr::sim::SeatId id) {
    for (const auto& seat : setup.seats) if (seat.seat_id == id) return seat.player;
    return 0;
}

// What a human seat does at its completed tick `tick`: ordinary ownership and orbit orders (some of them
// unauthorized, refused identically on every peer at execution), and the first seat pauses and resumes.
std::optional<galactic::CommandPayload> intent(const galactic::GalacticSetup& setup, const eawr::sim::SeatId seat,
    const std::uint64_t tick, const bool leader) {
    const auto planets = setup.planets.size();
    if (leader && tick == 11) return galactic::PauseGalaxy{};
    if (leader && tick == 19) return galactic::ResumeGalaxy{};
    if (leader && tick == 23) return galactic::SetSpeed{false, math::Fixed::from_raw(2 * math::Fixed::scale)};
    if (tick % 3 == (seat.value % 3))
        return galactic::DebugSetPlanetOwner{setup.planets[(tick * 7 + seat.value) % planets].id, player_of(setup, seat)};
    if (tick % 4 == 1)
        return galactic::DebugSetFleetOrbit{setup.fleets[(tick + seat.value) % setup.fleets.size()].id,
            setup.planets[(tick * 11) % planets].id};
    return std::nullopt;
}

struct Rig {
    struct Options {
        std::uint64_t delay{3}, final_tick{60};
        std::uint64_t completeness_timeout_ms{100'000};
        std::vector<std::size_t> workers{1};
        std::optional<eawr::testing::WanProfile> wan;
        bool monitor{true};
        bool departures{false};
        std::size_t observers{0}; // trailing peers are observers on AI seats
    };
    Rig(galactic::GalacticSetup initial, const Options rig_options)
        : setup(std::move(initial)), options(rig_options), humans(human_seats(setup)),
          network(static_cast<std::uint32_t>(humans.size() + rig_options.observers)) {
        const auto peers = static_cast<std::uint32_t>(humans.size() + options.observers);
        if (options.wan) network.emulate(*options.wan);
        std::vector<net::PeerSeat> seats;
        for (std::size_t peer = 0; peer < peers; ++peer) {
            if (peer < humans.size()) seats.push_back({humans[peer], true});
            else seats.push_back({eawr::sim::SeatId{static_cast<std::uint32_t>(3 + peer - humans.size())}, false});
        }
        for (net::PeerIndex peer = 0; peer < peers; ++peer) {
            executors.push_back(std::make_unique<eawr::platform::ThreadWorkerAdapter>(options.workers[peer % options.workers.size()]));
            games.push_back(std::make_unique<GameSession>(require(GameSession::create(setup))));
            if (peer != 0) games.back()->set_state_hasher(std::make_shared<eawr::platform::ThreadStateHasher>());
            adapters.push_back(std::make_unique<net::GalacticSessionAdapter>(*games.back(), *executors.back()));
            transports.push_back(std::make_unique<FakeTransport>(network, peer));
            net::DesyncMonitor* monitor = nullptr;
            if (options.monitor) {
                net::DesyncOptions monitor_options;
                monitor_options.input_delay = options.delay;
                monitor_options.exchange_every = 10;
                monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), monitor_options));
                monitor = monitors.back().get();
            }
            net::SessionOptions session_options;
            session_options.input_delay = options.delay;
            session_options.final_tick_count = options.final_tick;
            session_options.completeness_timeout_ms = options.completeness_timeout_ms;
            if (options.departures) session_options.unconfirmed_departure = net::UnconfirmedDeparture::continue_alone;
            sessions.push_back(require(net::LockstepSession::create(*adapters.back(), *transports.back(), seats, session_options, monitor)));
            if (options.departures)
                sessions.back().enable_departures([](eawr::sim::SeatCommandKey key) {
                    return net::encode_galactic_input(key, galactic::SeatLeave{});
                });
            sessions.back().set_stage_observer([this, peer](std::string_view stage, const net::SeatInput& input, std::uint64_t) {
                if (peer == 0 && stage == "seal") sealed_by_zero.push_back(input);
            });
        }
        hashes.resize(peers);
        inputs.resize(peers);
        submitted.assign(peers, std::numeric_limits<std::uint64_t>::max());
        departures.resize(peers);
    }

    // One fake millisecond per round, every peer submits its scripted intent once per completed tick.
    void run(const std::uint64_t max_rounds = 20'000, const std::function<void(std::uint64_t)>& each_round = {}) {
        for (std::uint64_t round = 0; round < max_rounds; ++round) {
            network.round = round;
            for (net::PeerIndex peer = 0; peer < sessions.size(); ++peer) {
                if (finished[peer]) continue;
                const auto tick = adapters[peer]->completed_tick();
                if (peer < humans.size() && tick < options.final_tick && submitted[peer] != tick) {
                    submitted[peer] = tick;
                    if (const auto action = intent(setup, humans[peer], tick, peer == 0)) {
                        const auto key = sessions[peer].submit([&](auto stamp) { return net::encode_galactic_input(stamp, *action); });
                        if (key) log.push_back({{key.value().tick, key.value().seat, key.value().sequence}, *action});
                    }
                }
                auto result = sessions[peer].pump(round);
                if (!result) { failure = result.error(); return; }
                for (const auto& key : result.value().departures) departures[peer].push_back(key);
                if (result.value().completed) {
                    hashes[peer].push_back(result.value().completed->state_sha256);
                    auto input = require(adapters[peer]->canonical_input());
                    inputs[peer].push_back(std::move(input));
                    static_cast<void>(adapters[peer]->take_tick());
                }
                if (adapters[peer]->completed_tick() == options.final_tick && (!options.monitor || monitors[peer]->agreed_end()))
                    finished[peer] = true;
            }
            if (each_round) each_round(round);
            if (std::all_of(finished.begin(), finished.begin() + static_cast<std::ptrdiff_t>(sessions.size()), [](const bool value) { return value; })) return;
        }
    }

    galactic::GalacticSetup setup;
    Options options;
    std::vector<eawr::sim::SeatId> humans;
    FakeNetwork network;
    std::vector<std::unique_ptr<eawr::platform::ThreadWorkerAdapter>> executors;
    std::vector<std::unique_ptr<GameSession>> games;
    std::vector<std::unique_ptr<net::GalacticSessionAdapter>> adapters;
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<net::LockstepSession> sessions;
    std::vector<std::vector<std::string>> hashes;
    std::vector<std::vector<Bytes>> inputs;
    std::vector<std::uint64_t> submitted;
    std::vector<std::vector<eawr::sim::SeatCommandKey>> departures;
    std::vector<galactic::GameCommand> log;
    std::vector<net::SeatInput> sealed_by_zero;
    std::array<bool, 16> finished{};
    std::optional<eawr::core::Diagnostic> failure;
};

// The unnetworked oracle: the same stamped commands in one GameSession.
std::vector<std::string> reference(const galactic::GalacticSetup& setup, std::vector<galactic::GameCommand> commands,
    const std::uint64_t ticks, const std::size_t workers) {
    std::sort(commands.begin(), commands.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    auto game = require(GameSession::create(setup));
    for (const auto& command : commands) require(game.submit(command));
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    std::vector<std::string> hashes;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = require(game.step(executor));
        hashes.push_back(stepped.state_sha256.empty() ? stepped.state_hash.get() : stepped.state_sha256);
    }
    return hashes;
}

void classification_and_codec() {
    const auto setup = versus_setup();
    GameSession game = require(GameSession::create(setup));
    const eawr::sim::InlineExecutor executor;
    const net::GalacticSessionAdapter adapter(game, executor);
    const auto stamp = [](std::uint32_t seat, std::uint64_t tick = 5) { return eawr::sim::SeatCommandKey{tick, {seat}, 1}; };
    const auto planet = setup.planets.front().id;
    const auto accepted = [&](std::uint32_t seat, const galactic::CommandPayload& payload) {
        return adapter.validate({stamp(seat), require(net::encode_galactic_input(stamp(seat), payload))});
    };

    expect(static_cast<bool>(accepted(1, galactic::PauseGalaxy{})), "a human seat may pause");
    expect(static_cast<bool>(accepted(2, galactic::DebugSetPlanetOwner{planet, 2})), "versus seat 2 is a human seat");
    expect(net::galactic_payload_authority(galactic::PostLoadFixup{}) == net::GalacticPayloadAuthority::engine_only
            && net::galactic_payload_authority(galactic::SeatRejoin{}) == net::GalacticPayloadAuthority::engine_only
            && net::galactic_payload_authority(galactic::ObserverCommand{}) == net::GalacticPayloadAuthority::engine_only
            && net::galactic_payload_authority(galactic::RecheckOrbit{}) == net::GalacticPayloadAuthority::engine_only
            && net::galactic_payload_authority(galactic::SeatLeave{}) == net::GalacticPayloadAuthority::human_wire,
        "payload classification");
    for (const auto& payload : std::vector<galactic::CommandPayload>{
             galactic::PostLoadFixup{}, galactic::SeatRejoin{}, galactic::RecheckOrbit{planet}}) {
        const auto refused = accepted(1, payload);
        expect(!refused && refused.error().code == net::session_errors::admission, "engine-only payloads never cross the human wire");
    }
    const auto rejoin = accepted(1, galactic::SeatRejoin{});
    expect(!rejoin && rejoin.error().message.find("rejoin is disabled") != std::string::npos,
        "Galactic rejoin refusal explains the M-GC1 policy");
    // Engine seats (AI seat 3) and unbound seats are not network seats.
    const auto ai = accepted(3, galactic::PauseGalaxy{});
    expect(!ai && ai.error().code == net::session_errors::admission, "an AI seat cannot send human wire input");
    const auto unbound = accepted(77, galactic::PauseGalaxy{});
    expect(!unbound && unbound.error().code == net::session_errors::admission, "an unbound seat is refused");
    // The child battle barrier is explicitly disabled (GCMP-05).
    const auto tactical = accepted(1, galactic::TacticalLayerCommand{1, 5, {}});
    expect(!tactical && tactical.error().code == net::session_errors::admission, "tactical-layer commands stay off until the barrier lands");
    // Debug commands follow the agreed setup flag.
    auto plain = setup;
    plain.debug_commands = false;
    GameSession plain_game = require(GameSession::create(plain));
    const net::GalacticSessionAdapter plain_adapter(plain_game, executor);
    const auto debug = plain_adapter.validate({stamp(1), require(net::encode_galactic_input(stamp(1), galactic::DebugSetPlanetOwner{planet, 1}))});
    expect(!debug && debug.error().code == net::session_errors::admission, "debug commands need the agreed setup flag");

    // Strict codec: noncanonical bytes are refused.
    auto bytes = require(net::encode_galactic_input(stamp(1), galactic::DebugSetPlanetOwner{planet, 1}));
    const auto decoded = require(net::decode_galactic_input({stamp(1), bytes}));
    expect(decoded.key.tick == 5 && decoded.payload == galactic::CommandPayload{galactic::DebugSetPlanetOwner{planet, 1}}, "codec round trip");
    auto longer = bytes;
    longer.push_back(0);
    expect(!net::decode_galactic_input({stamp(1), longer}), "trailing bytes are refused");
    auto bad_layer = bytes;
    bad_layer[0] = 9;
    expect(!net::decode_galactic_input({stamp(1), bad_layer}), "unknown layer is refused");
    auto bad_opcode = bytes;
    bad_opcode[1] = 0xff;
    expect(!net::decode_galactic_input({stamp(1), bad_opcode}), "unknown opcode is refused");
    expect(!net::decode_galactic_input({stamp(1), {}}), "empty input is refused");
    expect(!net::decode_galactic_input({stamp(0), bytes}), "seat zero is refused");

    expect(adapter.is_departure({stamp(1), require(net::encode_galactic_input(stamp(1), galactic::SeatLeave{}))}), "seat leave is the departure input");
    expect(!adapter.is_departure({stamp(1), bytes}), "ordinary input is not a departure");

    // install only takes the executing tick's complete, ordered inputs.
    GameSession target = require(GameSession::create(setup));
    net::GalacticSessionAdapter installer(target, executor);
    const net::SeatInput late{stamp(1, 3), require(net::encode_galactic_input(stamp(1, 3), galactic::PauseGalaxy{}))};
    expect(!installer.install(std::array{late}), "install refuses input stamped for another tick (unsealed or late)");
    const net::SeatInput now{stamp(1, 0), require(net::encode_galactic_input(stamp(1, 0), galactic::PauseGalaxy{}))};
    const net::SeatInput before{{0, {1}, 0}, now.bytes};
    expect(!installer.install(std::array{now, before}), "install refuses non-canonical order");
    const net::SeatInput unsupported_leave{stamp(1, 0),
        require(net::encode_galactic_input(stamp(1, 0), galactic::SeatLeave{}))};
    const auto no_policy = installer.install(std::array{unsupported_leave});
    expect(!no_policy && no_policy.error().message.find("seat-control fallback") != std::string::npos,
        "a missing dropout policy cannot silently remove a network voter without changing the galaxy");
    expect(static_cast<bool>(installer.install(std::array{now})), "install takes the executing tick");
    const auto stepped = require(installer.step());
    expect(stepped.completed_tick == 1 && installer.completed_tick() == 1, "one step is one tick");
    expect(target.galaxy().clock().pause_depth == 1, "the installed pause executed at its agreed tick");
    const auto input = require(installer.canonical_input());
    expect(!input.empty(), "canonical input records the executed command");
}

void loopback(const std::string& name, const galactic::GalacticSetup& setup, const std::optional<eawr::testing::WanProfile> wan) {
    std::optional<std::vector<std::string>> first_hashes;
    std::optional<Bytes> first_state;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        Rig::Options options;
        options.workers = {workers, 4U, 1U, 2U}; // peers deliberately mix worker counts
        options.wan = wan;
        Rig rig(setup, options);
        rig.run(60'000);
        const auto where = name + " workers=" + std::to_string(workers) + ": ";
        expect(!rig.failure, where + (rig.failure ? rig.failure->code + " " + rig.failure->message : ""));
        if (rig.failure) return;
        const auto oracle = reference(setup, rig.log, options.final_tick, workers);
        for (std::size_t peer = 0; peer < rig.sessions.size(); ++peer) {
            expect(rig.adapters[peer]->completed_tick() == options.final_tick, where + "every peer completes the agreed ticks and no more");
            expect(rig.hashes[peer] == rig.hashes[0], where + "equal per-tick digests on every peer");
            expect(rig.inputs[peer] == rig.inputs[0], where + "equal canonical input on every peer");
            expect(rig.monitors[peer]->agreed_end(), where + "the desync monitor agrees through the final tick");
            expect(rig.adapters[peer]->canonical_state() == rig.adapters[0]->canonical_state(), where + "equal final canonical state");
        }
        expect(rig.hashes[0] == oracle, where + "networked digests equal the unnetworked reference");
        expect(!rig.log.empty(), where + "commands were exchanged");
        if (workers == 1) { first_hashes = rig.hashes[0]; first_state = rig.adapters[0]->canonical_state(); }
        else {
            expect(rig.hashes[0] == *first_hashes, where + "digests equal the 1-worker run");
            expect(rig.adapters[0]->canonical_state() == *first_state, where + "canonical state equals the 1-worker run");
        }
        // Pause is session time: the clock froze while the tick counter went on to its end.
        if (workers == 1) {
            const auto& clock = rig.games[0]->galaxy().clock();
            expect(clock.pause_depth == 0, where + "the scripted resume released the pause");
        }
    }
}

void pause_progress_and_no_local_ticks() {
    const auto setup = versus_setup();
    Rig::Options options;
    options.final_tick = 40;
    Rig rig(setup, options);
    std::uint32_t paused_ticks = 0;
    rig.run(20'000, [&](std::uint64_t) {
        const auto& game = *rig.games[0];
        if (game.galaxy().clock().pause_depth > 0) ++paused_ticks;
    });
    expect(!rig.failure && paused_ticks > 0, "the pause window was observed");
    expect(rig.adapters[0]->completed_tick() == 40 && rig.adapters[1]->completed_tick() == 40,
        "session ticks, not galaxy time, are the shared steps: a pause adds none and removes none");
    expect(rig.hashes[0] == rig.hashes[1], "paused input progress keeps digests equal");

    // A lone peer takes no extra tick: without the other batch it cannot leave tick 0.
    auto lone_options = options;
    lone_options.monitor = false; // the batch wait, not the monitor's setup barrier, is what is observed here
    Rig lone(setup, lone_options);
    for (std::uint64_t round = 0; round < 500; ++round) {
        lone.network.round = round;
        const auto result = lone.sessions[0].pump(round);
        expect(static_cast<bool>(result), "waiting is not an error");
    }
    expect(lone.adapters[0]->completed_tick() == 0, "no local extra ticks while a sealed batch is missing");
    const auto& waiting = lone.sessions[0].waiting_for();
    expect(waiting && waiting->tick == 0 && waiting->peers == std::vector<net::PeerIndex>{1}, "the missing peer is named");
}

void held_and_duplicate_transport() {
    const auto setup = versus_setup();
    Rig::Options options;
    options.final_tick = 50;
    Rig rig(setup, options);
    // Peer 1's stream is held back for a while, delayed unevenly, and every batch is duplicated.
    rig.network.fault = [](net::PeerIndex from, net::PeerIndex, std::span<const std::uint8_t> frame) {
        FakeDelivery action;
        if (frame[0] == net::sealed_batch_kind) {
            action.duplicate = true;
            action.duplicate_delay = 3;
            if (from == 1) action.ordered_delay = 11;
        }
        return action;
    };
    rig.run(60'000);
    expect(!rig.failure, "held and duplicated stream");
    for (std::size_t peer = 0; peer < 2; ++peer) {
        expect(rig.adapters[peer]->completed_tick() == 50, "delay and duplicates add no shared step");
        expect(rig.hashes[peer] == rig.hashes[0], "delayed and duplicated batches keep digests equal");
    }
    expect(rig.hashes[0] == reference(setup, rig.log, 50, 1), "delayed run equals the reference");
}

void racing_inputs_are_canonical() {
    const auto setup = versus_setup();
    Rig::Options options;
    options.final_tick = 24;
    options.monitor = false;
    Rig rig(setup, options);
    const auto planet = setup.planets.front().id;
    // Both seats take the same planet at the same stamped tick; canonical order (tick, seat) decides, not arrival.
    std::array<bool, 2> sent{};
    rig.network.fault = [](net::PeerIndex from, net::PeerIndex, std::span<const std::uint8_t> frame) {
        FakeDelivery action;
        if (frame[0] == net::sealed_batch_kind && from == 0) action.ordered_delay = 5;
        return action;
    };
    for (std::uint64_t round = 0; round < 20'000; ++round) {
        rig.network.round = round;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            if (!sent[peer] && rig.adapters[peer]->completed_tick() == 6) {
                sent[peer] = true;
                require(rig.sessions[peer].submit([&](auto stamp) {
                    return net::encode_galactic_input(stamp, galactic::DebugSetPlanetOwner{planet, peer == 0 ? 1U : 2U});
                }));
            }
            if (rig.adapters[peer]->completed_tick() < 24) require(rig.sessions[peer].pump(round));
        }
        if (rig.adapters[0]->completed_tick() == 24 && rig.adapters[1]->completed_tick() == 24) break;
    }
    for (std::size_t peer = 0; peer < 2; ++peer) {
        const auto planets = rig.games[peer]->galaxy().snapshot()->planets();
        const auto found = std::find_if(planets.begin(), planets.end(), [&](const auto& value) { return value.id == planet; });
        expect(found != planets.end() && found->owner == 2, "the later seat in canonical order wins the same-tick race on every peer");
        expect(rig.games[peer]->state_sha256() == rig.games[0]->state_sha256(), "racing inputs end in equal state");
    }
}

void refusals_through_the_session() {
    const auto setup = versus_setup();
    const auto planet = setup.planets.front().id;
    const auto forge = [&](const std::uint32_t seat, const galactic::CommandPayload& payload, bool observer = false) {
        Rig::Options options;
        options.monitor = false;
        options.observers = observer ? 1 : 0;
        Rig rig(setup, options);
        // Peer 1 (or the observer) is a bare stream here; the frame is a validly framed sealed batch.
        const net::PeerIndex sender = observer ? 2 : 1;
        const eawr::sim::SeatCommandKey key{0, {seat}, 1};
        net::SealedBatch batch{sender, 0, 0, {{key, require(net::encode_galactic_input(key, payload))}}};
        const auto frame = require(net::encode_batch(batch));
        rig.network.deliver(sender, 0, frame);
        rig.network.round = 5;
        return rig.sessions[0].pump(5);
    };
    const auto wrong = forge(1, galactic::PauseGalaxy{});
    expect(!wrong && wrong.error().code == net::session_errors::admission, "a batch cannot issue for another peer's seat");
    const auto engine = forge(2, galactic::PostLoadFixup{});
    expect(!engine && engine.error().code == net::session_errors::admission, "an engine-only payload in a sealed batch fails the session");
    const auto observer_obs = forge(3, galactic::PauseGalaxy{}, true);
    expect(!observer_obs && observer_obs.error().code == net::session_errors::admission, "an observer seat cannot seal commands");
    const auto ai = forge(3, galactic::DebugSetPlanetOwner{planet, 3});
    expect(!ai && ai.error().code == net::session_errors::admission, "the sender binding refuses a stamp for a seat the peer does not own");

    // The observer peer cannot submit locally either.
    Rig::Options options;
    options.monitor = false;
    options.observers = 1;
    Rig rig(setup, options);
    const auto submitted = rig.sessions[2].submit([&](auto stamp) { return net::encode_galactic_input(stamp, galactic::PauseGalaxy{}); });
    expect(!submitted && submitted.error().code == net::session_errors::admission, "observers participate in completeness without input");

    // A late contradicting duplicate of an accepted batch is refused by the core.
    Rig duplicate_rig(setup, [] { Rig::Options o; o.monitor = false; return o; }());
    const eawr::sim::SeatCommandKey key{0, {2}, 1};
    const net::SealedBatch first{1, 0, 0, {{key, require(net::encode_galactic_input(key, galactic::PauseGalaxy{}))}}};
    const net::SealedBatch changed{1, 0, 0, {{key, require(net::encode_galactic_input(key, galactic::ResumeGalaxy{}))}}};
    duplicate_rig.network.deliver(1, 0, require(net::encode_batch(first)));
    duplicate_rig.network.deliver(1, 0, require(net::encode_batch(changed)));
    duplicate_rig.network.round = 9;
    const auto contradicted = duplicate_rig.sessions[0].pump(9);
    expect(!contradicted && contradicted.error().code == net::session_errors::conflicting_duplicate,
        "a contradicting replay of an accepted batch is refused");
}

void engine_input_runs_on_every_peer() {
    const auto setup = versus_setup();
    Rig::Options options;
    options.final_tick = 36;
    Rig rig(setup, options);
    std::size_t calls = 0;
    for (std::size_t peer = 0; peer < rig.adapters.size(); ++peer)
        rig.adapters[peer]->set_engine_source([&](const GameSession& game, std::uint64_t tick) {
            ++calls;
            std::vector<galactic::GameCommand> produced;
            // The AI seat acts deterministically from the executing tick and the world it can read.
            if (tick % 6 == 4)
                produced.push_back({{tick, {3}, tick + 1}, galactic::DebugSetPlanetOwner{game.galaxy().setup().planets[tick % 24].id, 3}});
            return eawr::core::Result<std::vector<galactic::GameCommand>>::success(std::move(produced));
        });
    rig.run(60'000);
    expect(!rig.failure && calls == 2 * 36, "the engine source runs once per executed tick on each peer");
    expect(rig.hashes[0] == rig.hashes[1] && rig.inputs[0] == rig.inputs[1], "engine input yields equal digests and canonical input");
    expect(std::any_of(rig.inputs[0].begin(), rig.inputs[0].end(), [](const Bytes& value) { return !value.empty(); }), "canonical input carries engine-side commands");
    // Engine commands never travel: peers sealed only their own seats' input.
    expect(std::none_of(rig.sealed_by_zero.begin(), rig.sealed_by_zero.end(), [](const net::SeatInput& input) { return input.key.seat.value == 3; }),
        "engine-seat input is never sealed on the wire");

    // An engine source cannot speak for a human seat.
    Rig bad(setup, options);
    bad.adapters[0]->set_engine_source([](const GameSession&, std::uint64_t tick) {
        std::vector<galactic::GameCommand> produced{{{tick, {1}, tick + 1}, galactic::PauseGalaxy{}}};
        return eawr::core::Result<std::vector<galactic::GameCommand>>::success(std::move(produced));
    });
    bad.run(5'000);
    expect(bad.failure && bad.failure->code == net::session_errors::admission, "an engine source for a human seat fails the step");
}

void departure_through_the_session() {
    const auto setup = policy_setup();
    Rig::Options options;
    options.final_tick = 40;
    options.monitor = false;
    options.departures = true;
    Rig rig(setup, options);
    bool left = false;
    for (std::uint64_t round = 0; round < 20'000 && !rig.failure; ++round) {
        rig.network.round = round;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            if (rig.adapters[peer]->completed_tick() >= 40) continue;
            if (peer == 1 && !left && rig.adapters[1]->completed_tick() == 10) {
                left = true;
                require(rig.sessions[1].submit([](auto stamp) { return net::encode_galactic_input(stamp, galactic::SeatLeave{}); }));
            }
            auto result = rig.sessions[peer].pump(round);
            if (!result) { rig.failure = result.error(); break; }
            for (const auto& key : result.value().departures) rig.departures[peer].push_back(key);
        }
        if (rig.adapters[0]->completed_tick() == 40) break;
    }
    expect(!rig.failure, "departure run");
    expect(rig.departures[0].size() == 1 && rig.departures[0][0].seat.value == 2 && rig.departures[0][0].tick == 13,
        "the leave executes at the agreed tick");
    const auto seats = rig.games[0]->galaxy().seats();
    const auto found = std::find_if(seats.begin(), seats.end(), [](const auto& seat) { return seat.seat_id.value == 2; });
    expect(found != seats.end() && !found->active, "the departed seat is inactive in the galaxy");
    // The survivor's session applied the recorded policy: the leaver's faction is now controlled by its AI fallback.
    const auto& control = rig.games[0]->galaxy().seat_control();
    bool ai_took_over = false;
    if (control)
        for (const auto& controller : control->control.controllers)
            if (controller.player == 2) ai_took_over = controller.kind == galactic::FactionControl::ai;
    expect(ai_took_over, "the AI fallback takes over the departed seat's faction");
    expect(rig.adapters[0]->completed_tick() == 40, "the survivor continues alone");
}

// GCMP-09 / WGS-P02/P04: staggered delivery and mixed worker counts must not turn local loss detection into
// a local SeatLeave. Compare every surviving peer through the agreed cutoff, then leave the rest of the coop team.
void agreed_departure_policy(const std::uint32_t humans, const bool disconnect) {
    const auto setup = humans == 2 ? policy_setup() : coop_policy_setup(humans);
    Rig::Options options;
    options.final_tick = 64;
    options.departures = true;
    options.workers = {1, 2, 4, 8};
    Rig rig(setup, options);
    rig.network.fault = [](net::PeerIndex from, net::PeerIndex to, std::span<const std::uint8_t>) {
        FakeDelivery action;
        action.ordered_delay = (from * 3 + to * 5) % 7;
        return action;
    };
    const net::PeerIndex leaver = humans == 2 ? 1U : 0U; // versus opponent / coop lead
    const net::PeerIndex survivor = humans == 2 ? 0U : humans - 1;
    std::vector<bool> inactive(humans);
    std::optional<eawr::sim::SeatCommandKey> submitted_leave;
    std::optional<net::DepartureEvent> agreed;
    std::vector<std::vector<net::DepartureEvent>> events(humans);
    bool triggered = false, rest_leaving = false, checked_lead = false;
    std::optional<eawr::sim::SeatCommandKey> last_leave;
    for (std::uint64_t round = 0; round < 50'000; ++round) {
        rig.network.round = round;
        if (!triggered && rig.adapters[leaver]->completed_tick() >= 12) {
            triggered = true;
            if (disconnect) {
                rig.network.open[leaver] = false;
                inactive[leaver] = true;
            } else {
                submitted_leave = require(rig.sessions[leaver].submit([](auto key) {
                    return net::encode_galactic_input(key, galactic::SeatLeave{});
                }));
            }
        }
        for (net::PeerIndex peer = 0; peer < humans; ++peer) {
            if (inactive[peer] || (rig.adapters[peer]->completed_tick() == options.final_tick
                && rig.monitors[peer]->agreed_end())) continue;
            const auto before = rig.adapters[peer]->completed_tick();
            const auto result = require(rig.sessions[peer].pump(round));
            if (result.completed) {
                rig.hashes[peer].push_back(result.completed->state_sha256);
                rig.inputs[peer].push_back(require(rig.adapters[peer]->canonical_input()));
                static_cast<void>(rig.adapters[peer]->take_tick());
            }
            for (const auto& event : result.departure_events) {
                expect(result.completed && result.completed->completed_tick == event.key.tick + 1
                    && before == event.key.tick, "departure is reported only after its agreed simulation tick");
                events[peer].push_back(event);
                if (event.key.seat == rig.humans[leaver]) {
                    if (!agreed) agreed = event;
                    expect(event == *agreed, "survivors agree the exact leave stamp and cause despite unequal delivery");
                    if (submitted_leave) expect(event.key == *submitted_leave, "ordinary leave retains its original agreed stamp");
                    if (humans > 2) {
                        const auto& policy = rig.games[peer]->galaxy().seat_control()->control;
                        expect(policy.teams.front().lead == rig.humans[1], "coop lead moves to the lowest remaining seat");
                        expect(policy.controllers.front().kind == galactic::FactionControl::human,
                            "a remaining coop human retains faction control");
                        checked_lead = true;
                    }
                }
                if (event.key.seat == rig.humans[peer]) inactive[peer] = true;
            }
        }
        if (agreed && !rest_leaving && rig.adapters[survivor]->completed_tick() >= 32) {
            rest_leaving = true;
            // The last survivor leaves after the other ordinary quits have reached every active peer.
            if (humans > 2)
                for (net::PeerIndex peer = 1; peer < survivor; ++peer)
                    require(rig.sessions[peer].submit([](auto key) { return net::encode_galactic_input(key, galactic::SeatLeave{}); }));
        }
        if (humans > 2 && !last_leave && rig.adapters[survivor]->completed_tick() >= 48)
            last_leave = require(rig.sessions[survivor].submit([](auto key) {
                return net::encode_galactic_input(key, galactic::SeatLeave{});
            }));
        if (inactive[survivor]) break;
        if (rig.adapters[survivor]->completed_tick() == options.final_tick && rig.monitors[survivor]->agreed_end()) break;
    }
    expect(triggered && agreed.has_value(), "mid-match loss produced an agreed departure");
    if (!agreed) return;
    expect(agreed->reason == (disconnect ? net::DepartureReason::disconnect : net::DepartureReason::quit), "agreed departure cause");
    if (humans == 2)
        expect(rig.adapters[survivor]->completed_tick() == options.final_tick && rig.monitors[survivor]->agreed_end(),
            "versus survivor continues and agrees its final digest");
    else
        expect(last_leave && rig.adapters[survivor]->completed_tick() == last_leave->tick + 1,
            "the remaining coop human can keep playing and then quit at its agreed tick");
    for (net::PeerIndex peer = 0; peer < humans; ++peer) {
        if (disconnect && peer == leaver) continue;
        expect(!events[peer].empty(), "each live peer applied a departure");
        const auto common = std::min(rig.hashes[peer].size(), rig.hashes[survivor].size());
        expect(common > agreed->key.tick && std::equal(rig.hashes[peer].begin(),
            rig.hashes[peer].begin() + static_cast<std::ptrdiff_t>(common), rig.hashes[survivor].begin()),
            "surviving digests are equal before, at and after the agreed leave tick");
        expect(std::equal(rig.inputs[peer].begin(), rig.inputs[peer].begin() + static_cast<std::ptrdiff_t>(common),
            rig.inputs[survivor].begin()), "canonical inputs agree through every common tick");
    }
    const auto& control = rig.games[survivor]->galaxy().seat_control()->control;
    if (humans == 2) {
        expect(control.controllers[1].kind == galactic::FactionControl::ai, "D5: versus continues with configured AI fallback");
        const auto seats = rig.games[survivor]->galaxy().seats();
        expect(std::any_of(seats.begin(), seats.end(), [](const auto& seat) { return seat.player == 2
            && seat.kind == eawr::sim::SeatKind::ai && seat.active; }), "versus fallback issuer is active");
    } else {
        expect(checked_lead && control.teams.front().lead.value == 0
            && control.controllers.front().kind == galactic::FactionControl::ai,
            "last coop human departure clears the lead and activates configured AI fallback");
        const auto seats = rig.games[survivor]->galaxy().seats();
        expect(std::any_of(seats.begin(), seats.end(), [](const auto& seat) { return seat.player == 1
            && seat.kind == eawr::sim::SeatKind::ai && seat.active; }), "coop fallback issuer is active");
        expect(control.controllers.front().configuration == setup.seat_control->controllers.front().configuration
            && control.controllers.front().state == setup.seat_control->controllers.front().state,
            "departure retains the configured controller and persistent state");
    }
    const auto before_rejoin = rig.adapters[survivor]->state_hash();
    const eawr::sim::SeatCommandKey rejoin_key{rig.adapters[survivor]->completed_tick(), rig.humans[leaver], 999};
    const net::SeatInput rejoin{rejoin_key, require(net::encode_galactic_input(rejoin_key, galactic::SeatRejoin{}))};
    const auto refused = rig.adapters[survivor]->validate(rejoin);
    expect(!refused && refused.error().message.find("rejoin is disabled") != std::string::npos,
        "a departed Galactic seat cannot rejoin and receives a clear diagnostic");
    expect(!rig.adapters[survivor]->install(std::array{rejoin}), "a rejoin cannot bypass admission through install");
    expect(rig.adapters[survivor]->state_hash() == before_rejoin, "refused rejoin changes no agreed state");
}

void two_peer_split_continues_separately() {
    Rig::Options options;
    options.final_tick = 80;
    options.departures = true;
    options.workers = {1, 8};
    Rig rig(policy_setup(), options);
    for (std::uint64_t round = 0; round < 40; ++round) {
        rig.network.round = round;
        for (auto& session : rig.sessions) require(session.pump(round));
    }
    rig.network.open[0] = rig.network.open[1] = false;
    for (std::uint64_t round = 40; round < 2000; ++round) {
        rig.network.round = round;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            const auto result = require(rig.sessions[peer].pump(round));
            for (const auto& event : result.departure_events) rig.departures[peer].push_back(event.key);
        }
        if (rig.monitors[0]->agreed_end() && rig.monitors[1]->agreed_end()) break;
    }
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        const auto other = 1U - peer;
        expect(rig.adapters[peer]->completed_tick() == options.final_tick && rig.monitors[peer]->agreed_end(),
            "D5: each side of a two-peer link split can continue its separate campaign");
        expect(rig.departures[peer].size() == 1 && rig.departures[peer].front().seat == rig.humans[other],
            "each split campaign applies one leave for the unreachable human");
        const auto& policy = rig.games[peer]->galaxy().seat_control()->control;
        expect(policy.controllers[peer].kind == galactic::FactionControl::human
            && policy.controllers[other].kind == galactic::FactionControl::ai,
            "each split campaign retains its own human and activates the other's configured fallback");
        const eawr::sim::SeatCommandKey key{options.final_tick, rig.humans[other], 999};
        const auto rejoin = rig.adapters[peer]->validate({key, require(net::encode_galactic_input(key, galactic::SeatRejoin{}))});
        expect(!rejoin && rejoin.error().message.find("rejoin is disabled") != std::string::npos,
            "D8: split campaigns cannot reconcile by rejoining a departed seat");
    }
}

void multiple_survivors_keep_confirmation_policy() {
    Rig::Options options;
    options.final_tick = 80;
    options.departures = true;
    options.monitor = false; // isolate the departure agreement from the hash gate
    options.completeness_timeout_ms = 20;
    Rig rig(coop_policy_setup(3), options);
    for (std::uint64_t round = 0; round < 40; ++round) {
        rig.network.round = round;
        for (auto& session : rig.sessions) require(session.pump(round));
    }
    std::array<bool, 3> failed{};
    bool departed = false;
    // Peer 0 keeps an open stream but stops producing input. continue_alone must not waive the
    // existing timeout/confirmation policy when two survivors still participate in the agreement.
    for (std::uint64_t round = 40; round < 240 && !(failed[1] && failed[2]); ++round) {
        rig.network.round = round;
        for (net::PeerIndex peer = 1; peer < 3; ++peer) {
            if (failed[peer]) continue;
            const auto result = rig.sessions[peer].pump(round);
            if (!result) {
                failed[peer] = true;
                expect(result.error().code == net::session_errors::stalled,
                    "multiple-survivor timeout still reports unconfirmed connection loss");
            } else departed = departed || !result.value().departure_events.empty();
        }
    }
    expect(failed[1] && failed[2] && !departed, "continue_alone cannot bypass the existing multiple-survivor confirmation rule");
    for (net::PeerIndex peer = 1; peer < 3; ++peer) {
        const auto seats = rig.games[peer]->galaxy().seats();
        expect(seats.front().active, "an unconfirmed multi-peer timeout does not synthesize a leave");
    }
}

} // namespace

int main() {
    try {
        classification_and_codec();
        loopback("versus", versus_setup(), std::nullopt);
        loopback("versus-wan", versus_setup(), eawr::testing::wan_profiles[1]);
        loopback("coop-4-seats", coop_setup(), std::nullopt);
        pause_progress_and_no_local_ticks();
        held_and_duplicate_transport();
        racing_inputs_are_canonical();
        refusals_through_the_session();
        engine_input_runs_on_every_peer();
        departure_through_the_session();
        for (const auto humans : {2U, 3U, 4U})
            for (const auto disconnect : {false, true}) agreed_departure_policy(humans, disconnect);
        two_peer_split_continues_separately();
        multiple_survivors_keep_confirmation_policy();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}
