#include "lockstep_session_fixtures.hpp"
#include "fake_transport.hpp"
#include "eawr/net/tactical_adapter.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/session_monitor.hpp"
#include "eawr/net/tactical_input.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lockstep_session_test {
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
using eawr::testing::FakeDelivery;
using eawr::testing::FakeNetwork;
using eawr::testing::FakeTransport;
using sim::math::Fixed;

struct DepartureMatch {
    DepartureMatch(unsigned peers, bool human_ally = false, bool ai_ally = false, unsigned workers = 1, bool monitored = false, std::uint64_t monitor_every = 2,
        net::SessionMonitor* gate = nullptr, unsigned gate_peer = 2, std::uint64_t completeness_timeout_ms = 100)
        : network(peers), executor(workers) {
        initial.seed = 715;
        initial.departure_policy.emplace();
        for (unsigned peer = 0; peer < peers; ++peer) {
            const auto id = peer + 1;
            initial.players.push_back({id, human_ally && id == 3 ? 1U : id, id, tactical::player_flag_commandable});
            initial.departure_policy->humans.push_back(id);
            roster.push_back({sim::SeatId{id}, id, true, false});
            tactical::UnitState unit;
            unit.entity_id = id; unit.type_id = 1; unit.owner = id;
            unit.rotation = sim::math::identity_quat();
            initial.units.push_back(unit);
        }
        if (ai_ally) {
            const auto id = peers + 1;
            initial.players.push_back({id, 1, id, tactical::player_flag_commandable});
        }
        net::SessionOptions options;
        options.final_tick_count = 40; options.completeness_timeout_ms = completeness_timeout_ms;
        for (unsigned peer = 0; peer < peers; ++peer) {
            transports.push_back(std::make_unique<FakeTransport>(network, peer));
            if (monitored) {
                net::DesyncOptions monitor_options; monitor_options.exchange_every = monitor_every; monitor_options.input_delay = options.input_delay;
                monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), monitor_options));
            }
            sessions.push_back(require(net::TacticalLockstepSession::create(initial, {}, *transports.back(), executor, roster,
                options, monitored ? monitors.back().get() : (peer == gate_peer ? gate : nullptr))));
            sessions.back().set_takeover_handler([&, peer](tactical::PlayerId player, std::uint64_t tick) {
                attached[peer].push_back({player, tick});
                return eawr::core::Result<void>::success();
            });
            // Stand-in for the existing AI host: ordinary engine commands, generated
            // deterministically on every peer in the gated step and captured in replay.
            sessions.back().set_engine_step([](auto& world, const auto& workers_adapter) {
                for (const auto player : world.basic_ai_players()) {
                    auto submitted = world.submit({{world.completed_tick(), player, 1'000'000 + world.completed_tick()},
                        {player}, tactical::StopPayload{}});
                    if (!submitted) return eawr::core::Result<tactical::TacticalTick>::failure(submitted.error());
                }
                return world.step(workers_adapter);
            });
        }
    }
    void replay(unsigned peer) {
        const auto recorded = sessions[peer].simulation().record();
        const auto encoded = require(tactical::write_replay(recorded));
        expect(require(tactical::parse_replay(encoded)) == recorded, "departure policy and inputs codec roundtrip");
        auto replayed = require(tactical::TacticalSession::from_replay(recorded));
        while (replayed.completed_tick() < recorded.final_tick_count) require(replayed.step(executor));
        expect(replayed.state_sha256() == sessions[peer].simulation().state_sha256(), "quit/takeover replay reproduces authoritative state");
    }
    tactical::TacticalSetup initial;
    FakeNetwork network;
    eawr::platform::ThreadWorkerAdapter executor;
    std::vector<tactical::SeatBinding> roster;
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<net::TacticalLockstepSession> sessions;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::array<std::vector<std::pair<tactical::PlayerId, std::uint64_t>>, 8> attached;
};

void allied_last_human_is_named_winner() {
    auto initial = setup();
    initial.players[1].team_id = initial.players[0].team_id;
    initial.departure_policy = tactical::MultiplayerDeparturePolicy{{1, 2}};
    auto world = require(tactical::TacticalSession::create(initial, content()));
    require(world.submit({{0, 1, 0}, {}, tactical::QuitPayload{}}));
    sim::InlineExecutor executor;
    require(world.step(executor));
    expect(world.outcome() && world.outcome()->winner == 2
        && world.outcome()->winner_team == initial.players[1].team_id,
        "sole remaining allied human is the named winner rather than the quitter");
}

void ai_slot_does_not_hold_one_human_match() {
    DepartureMatch match(2, false, true);
    const auto quit = require(match.sessions[0].submit({{}, tactical::QuitPayload{}}));
    for (std::uint64_t round = 0; round < 20; ++round) {
        match.network.round = round;
        for (auto& session : match.sessions) require(session.pump(round));
    }
    const auto& outcomes = match.sessions[1].departure_outcomes();
    expect(outcomes.size() == 1 && outcomes.front().event.key == quit
        && outcomes.front().state == net::DepartureState::won_by_departure,
        "two humans plus AI: remaining human wins by departure under LAN MVP policy");
    expect(match.sessions[1].simulation().units().front().owner == 3,
        "allied AI ownership transfer still precedes the one-human match result");
    match.replay(1);
}

void partially_delivered_quit() {
    DepartureMatch match(3);
    const auto quit = require(match.sessions[0].submit({{}, tactical::QuitPayload{}}));
    match.network.fault = [](auto from, auto to, auto frame) {
        FakeDelivery delivery;
        delivery.drop = from == 0 && to == 2 && frame.front() == net::sealed_batch_kind && batch_tick(frame) >= 3;
        return delivery;
    };
    for (std::uint64_t round = 0; round < 20; ++round) {
        match.network.round = round;
        for (auto& session : match.sessions) require(session.pump(round));
    }
    expect(match.sessions[1].departure_outcomes().size() == 1 && match.sessions[2].departure_outcomes().empty(),
        "one survivor already executed the quit while another missed that batch");
    match.network.open[0] = false;
    for (std::uint64_t round = 20; round < 100; ++round) {
        match.network.round = round;
        for (unsigned peer = 1; peer < 3; ++peer) require(match.sessions[peer].pump(round));
    }
    for (unsigned peer = 1; peer < 3; ++peer) {
        const auto& outcomes = match.sessions[peer].departure_outcomes();
        expect(outcomes.size() == 1 && outcomes.front().event.key == quit
            && outcomes.front().event.reason == net::DepartureReason::quit, "recovery preserves the ordinary quit stamp and reports it only once");
    }
    expect(match.sessions[1].simulation().record() == match.sessions[2].simulation().record()
        && match.sessions[1].simulation().state_sha256() == match.sessions[2].simulation().state_sha256(),
        "already-departed survivor relays original quit and agrees with recovered survivor");
}

void departure_wire_refusals() {
    const std::vector<std::uint8_t> proposal{net::departure_proposal_kind, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t size = 0; size < proposal.size(); ++size) {
        DepartureMatch match(3);
        match.network.inbox[2].push_back({0, {1, {proposal.begin(), proposal.begin() + static_cast<std::ptrdiff_t>(size)}}});
        const auto pumped = match.sessions[2].pump(0);
        expect(!pumped && match.sessions[2].simulation().completed_tick() == 0, "truncated departure proposal is a typed refusal without stepping");
    }
    for (const auto kind : {net::departure_recovery_kind, net::departure_ready_kind, net::departure_ack_kind}) {
        DepartureMatch match(3);
        match.network.inbox[2].push_back({0, {1, {kind, 0, 0, 0, 0}}});
        expect(!match.sessions[2].pump(0), "departure data before a voter proposal is refused");
    }
    for (std::uint8_t kind = 52; kind <= 55; ++kind) {
        DepartureMatch match(3);
        match.network.inbox[2].push_back({0, {1, {kind}}});
        const auto rejected = match.sessions[2].pump(0);
        expect(!rejected && rejected.error().code == net::departure_errors::malformed,
            "unknown reserved departure kind is a departure-domain malformed refusal");
    }
    for (const auto offset : {1U, 5U, 13U}) {
        DepartureMatch match(3);
        auto invalid = proposal; invalid[offset] = 255;
        match.network.inbox[2].push_back({0, {1, invalid}});
        expect(!match.sessions[2].pump(0), "invalid target/reason/frontier cannot install a departure");
    }
}

void monitored_maximum_lead() {
    for (const unsigned peers : {2U, 3U}) {
        DepartureMatch match(peers, false, false, 4, true, 1000);
        for (unsigned peer = 1; peer < peers; ++peer) require(match.sessions[peer].pump(0));
        for (std::uint64_t round = 1; round <= 5; ++round) {
            match.network.round = round;
            require(match.sessions[0].pump(round));
        }
        expect(match.sessions[0].simulation().completed_tick() == 4, "departing sender reaches lead D+1");
        match.network.open[0] = false;
        if (peers == 2) {
            bool lost{};
            for (std::uint64_t round = 6; round < 160 && !lost; ++round) {
                match.network.round = round;
                const auto result = match.sessions[1].pump(round);
                if (!result) { lost = true; expect(result.error().code == net::session_errors::stalled, "maximum-lead lone survivor gets connection loss"); }
            }
            expect(lost && match.sessions[1].departure_outcomes().empty() && !match.sessions[1].simulation().outcome(),
                "maximum receive frontier cannot confirm a quitter or award a lone survivor the win");
            match.replay(1);
            continue;
        }
        for (std::uint64_t round = 6; round < 160; ++round) {
            match.network.round = round;
            for (unsigned peer = 1; peer < peers; ++peer) require(match.sessions[peer].pump(round));
        }
        for (unsigned peer = 1; peer < peers; ++peer) {
            const auto& events = match.sessions[peer].departure_outcomes();
            expect(events.size() == 1 && events.front().event.key.tick == net::sealed_input_horizon(3),
                "monitored maximum receive frontier agrees cutoff 2D+2");
            expect(match.sessions[peer].simulation().completed_tick() == 40 && match.monitors[peer]->agreed_end(),
                "maximum-lead monitored survivors finish without boundary refusal");
            match.replay(peer);
        }
        if (peers == 3) expect(match.sessions[1].simulation().state_sha256() == match.sessions[2].simulation().state_sha256(),
            "maximum-lead three-peer survivors agree after takeover");
    }
}

void monitor_send_close_race() {
    DepartureMatch match(2, false, false, 4, true);
    bool closed = false;
    match.network.before_send = [&](auto from, auto to, auto frame) {
        if (!closed && from == 0 && to == 1 && frame.front() == net::desync_combined_kind) {
            match.network.open[1] = false;
            closed = true;
        }
    };
    bool lost{};
    for (std::uint64_t round = 0; round < 100 && !lost; ++round) {
        match.network.round = round;
        const auto result = match.sessions[0].pump(round);
        if (!result) { lost = true; expect(result.error().code == net::session_errors::stalled, "hash-send closure is connection loss"); }
    }
    expect(closed && lost && match.sessions[0].departure_outcomes().empty() && !match.sessions[0].simulation().outcome(),
        "closed hash send cannot manufacture a departure or a winner from one local vote");
    match.replay(0);
}

void monitored_silent_peer() {
    DepartureMatch match(2, false, false, 4, true);
    for (std::uint64_t round = 0; round < 20; ++round) {
        match.network.round = round;
        for (auto& session : match.sessions) require(session.pump(round));
    }
    // The link stays open (transport keepalives), but the app stops sealing input
    // and hash rows. The pending exchange must not mask its liveness deadline.
    bool lost{};
    for (std::uint64_t round = 20; round < 200 && !lost; ++round) {
        match.network.round = round;
        const auto result = match.sessions[1].pump(round);
        if (!result) { lost = true; expect(result.error().code == net::session_errors::stalled, "silent hash voter has a bounded connection-loss deadline"); }
    }
    expect(lost && match.network.open[0] && match.sessions[1].departure_outcomes().empty()
        && !match.sessions[1].simulation().outcome(), "healthy keepalives cannot turn missing application input into an invented win");
    expect(!match.monitors[1]->frozen(), "silence is not relabelled as a state-hash divergence");
    match.replay(1);
}

void monitored_departures() {
    for (const unsigned peers : {2U, 3U}) {
        DepartureMatch match(peers, false, false, 4, true);
        for (std::uint64_t round = 0; round < 20; ++round) {
            match.network.round = round;
            for (auto& session : match.sessions) require(session.pump(round));
        }
        expect(match.sessions[1].simulation().completed_tick() > 0, "concrete monitor tick-zero barrier agrees");
        match.network.open[0] = false;
        if (peers == 2) {
            bool lost{};
            for (std::uint64_t round = 20; round < 160 && !lost; ++round) {
                match.network.round = round;
                const auto result = match.sessions[1].pump(round);
                if (!result) { lost = true; expect(result.error().code == net::session_errors::stalled, "monitored lone-survivor disconnect is connection loss"); }
            }
            expect(lost && match.sessions[1].departure_outcomes().empty() && !match.sessions[1].simulation().outcome(),
                "monitor membership cannot be retired by the only remaining local vote");
            match.replay(1);
            continue;
        }
        for (std::uint64_t round = 20; round < 160; ++round) {
            match.network.round = round;
            for (unsigned peer = 1; peer < peers; ++peer) require(match.sessions[peer].pump(round));
        }
        for (unsigned peer = 1; peer < peers; ++peer) {
            expect(match.sessions[peer].simulation().completed_tick() == 40, "concrete monitor releases departed-peer gates through match end");
            expect(match.sessions[peer].departure_outcomes().size() == 1, "monitored disconnect yields one outcome");
            expect(!match.monitors[peer]->frozen() && match.monitors[peer]->agreed_end(), "remaining monitor roster agrees including takeover AI inputs");
        }
        if (peers == 3) expect(match.sessions[1].simulation().state_sha256() == match.sessions[2].simulation().state_sha256(),
            "3->2 monitored survivors remain hash-equal after basic takeover");
    }
}

std::string chained_tactical_departures(unsigned workers, bool settling) {
    DepartureGate gate; gate.final_tick = 40;
    DepartureMatch match(4, false, false, workers, false, 2, &gate);
    bool early_second_proposal{};
    match.network.fault = [settling](auto from, auto to, auto bytes) {
        FakeDelivery delivery;
        if (settling && from == 1 && to == 2 && bytes.front() == net::departure_ack_kind && bytes[1] == 0)
            delivery.ordered_delay = 20;
        return delivery;
    };
    match.network.before_send = [&](auto from, auto to, auto bytes) {
        if (from == 3 && to == 2 && bytes.front() == net::departure_proposal_kind && bytes[1] == 1
            && gate.settled.empty()) early_second_proposal = true;
    };
    std::uint64_t round{};
    auto cycle = [&] {
        match.network.round = round;
        for (unsigned peer = 0; peer < 4; ++peer)
            if (match.network.open[peer]) require(match.sessions[peer].pump(round));
        ++round;
    };
    for (unsigned i = 0; i < 8; ++i) cycle();
    match.network.open[0] = false;
    for (unsigned i = 0; i < 8; ++i) cycle();
    expect(gate.settled.size() == (settling ? 0U : 1U) && match.sessions[2].simulation().completed_tick() == 0
        && match.sessions[2].departure_outcomes().empty(), "tactical first departure is agreed before its cutoff executes");
    match.network.open[1] = false;
    for (unsigned i = 0; i < 40; ++i) cycle();
    if (settling) expect(early_second_proposal, "second departure arrives inside the first settle window");
    expect(gate.settled.size() == 2 && gate.settled[0].first == 0 && gate.settled[1].first == 1
        && gate.settled[0].second <= gate.settled[1].second, "tactical departures agree in cutoff order without a retired voter");
    gate.allowed = true;
    for (unsigned i = 0; i < 150; ++i) cycle();
    for (unsigned peer = 2; peer < 4; ++peer) {
        const auto& events = match.sessions[peer].departure_outcomes();
        expect(events.size() == 2 && events[0].event.key.seat == sim::SeatId{1} && events[1].event.key.seat == sim::SeatId{2},
            "tactical survivors apply both departures exactly once in order");
        expect(match.sessions[peer].simulation().completed_tick() == 40, "tactical chained departures reach match end");
        match.replay(peer);
    }
    expect(match.sessions[2].simulation().record() == match.sessions[3].simulation().record()
        && match.sessions[2].simulation().state_sha256() == match.sessions[3].simulation().state_sha256(),
        "chained departure replay and survivor hashes agree");
    return match.sessions[2].simulation().state_sha256();
}

std::string retained_timing_departure(unsigned workers, bool host_leaves, bool late_ready,
    bool double_loss, bool expire) {
    constexpr unsigned count = 4;
    const std::uint64_t hold_tick = late_ready ? 130U : 140U;
    FakeNetwork network(count);
    eawr::platform::ThreadWorkerAdapter executor(workers);
    const auto roster = bindings(count);
    std::array<DepartureGate, count> gates;
    std::array<unsigned, count> retained{}, departures{};
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::TacticalSessionAdapter>> adapters;
    std::vector<net::LockstepSession> sessions;
    std::vector<net::PeerSeat> seats;
    for (const auto& binding : roster) seats.push_back({binding.seat, true});
    for (unsigned peer = 0; peer < count; ++peer) {
        gates[peer].allowed = true; gates[peer].final_tick = 240;
        transports.push_back(std::make_unique<FakeTransport>(network, peer));
        adapters.push_back(std::make_unique<net::TacticalSessionAdapter>(
            require(tactical::TacticalSession::create(setup(), content())), executor, roster));
        net::SessionOptions options;
        options.input_delay = 4; options.final_tick_count = 240; options.completeness_timeout_ms = late_ready ? 1000 : 100;
        options.non_lan = true;
        options.adaptive_timing = net::AdaptiveTiming{0, true, [](auto) { return std::optional<std::uint64_t>{330}; }};
        options.retain_timing_event = [&, peer](auto tick, auto) {
            expect(tick == 165, "retained timing activation is immutable");
            ++retained[peer]; return eawr::core::Result<void>::success();
        };
        sessions.push_back(require(net::LockstepSession::create(*adapters.back(), *transports.back(), seats, options, &gates[peer])));
        sessions.back().enable_departures([&](auto key) {
            const auto& binding = roster[key.seat.value - 1];
            return net::encode_tactical_input(key, binding, {{}, tactical::QuitPayload{}});
        });
    }
    bool late_vote{};
    network.fault = [&](auto from, auto to, auto bytes) {
        FakeDelivery delivery;
        if (late_ready && from == 0 && to == 1 && bytes.front() == net::timing_ready_kind) {
            delivery.ordered_delay = 120;
            delivery.drop = expire;
            late_vote = true;
        }
        if (double_loss && from == 2 && to == 1 && bytes.front() == net::departure_ack_kind && bytes[1] == 0)
            delivery.ordered_delay = 20;
        return delivery;
    };
    std::uint64_t round{};
    auto cycle = [&] {
        network.round = round;
        for (unsigned peer = 0; peer < count; ++peer) if (network.open[peer]) {
            if (adapters[peer]->completed_tick() == hold_tick) gates[peer].allowed = false;
            const auto result = require(sessions[peer].pump(round));
            departures[peer] += static_cast<unsigned>(result.departure_events.size());
        }
        ++round;
    };
    for (unsigned i = 0; i < (late_ready ? 190U : 300U); ++i) cycle();
    for (unsigned peer = 0; peer < count; ++peer)
        expect(retained[peer] == (late_ready && peer == 1 ? 0U : 1U)
            && adapters[peer]->completed_tick() == hold_tick && sessions[peer].input_delay() == 4,
            "every peer retained the timing event before the loss");
    if (late_ready) expect(late_vote, "loss races an ordered timing readiness vote");
    const unsigned target = host_leaves ? 0U : 2U;
    network.open[target] = false;
    if (expire) {
        std::array<bool, count> refused{};
        for (unsigned i = 0; i < 1200; ++i) {
            network.round = round;
            for (unsigned peer = 1; peer < count; ++peer) if (!refused[peer]) {
                const auto result = sessions[peer].pump(round);
                if (!result) {
                    refused[peer] = true;
                    expect(result.error().code == (peer == 1 ? net::timing_errors::timeout : net::departure_errors::timeout),
                        "unretained timing and retained departure expire with their respective diagnostics");
                }
            }
            ++round;
        }
        std::string hash;
        for (unsigned peer = 1; peer < count; ++peer) {
            expect(refused[peer] && adapters[peer]->completed_tick() == hold_tick && departures[peer] == 0
                && sessions[peer].input_delay() == 4, "every surviving peer refuses without advancing or hanging at expiry");
            expect(!sessions[peer].pump(round + 10000), "expiry refusal remains latched on every survivor");
            if (hash.empty()) hash = adapters[peer]->state_hash();
            expect(hash == adapters[peer]->state_hash(), "expiry code differences preserve equal frozen hashes");
        }
        return hash;
    }
    if (double_loss) {
        expect(host_leaves, "double-loss case first loses the host");
        for (unsigned i = 0; i < 8; ++i) cycle();
        expect(gates[1].settled.empty(), "adaptive double loss begins inside the first settle window");
        network.open[2] = false;
    }
    for (unsigned i = 0; i < 80; ++i) cycle();
    for (unsigned peer = 0; peer < count; ++peer) if (network.open[peer]) {
        expect(gates[peer].settled.size() == (double_loss ? 2U : 1U) && departures[peer] == 0,
            "departure settles before retained timing executes");
        gates[peer].allowed = true;
    }
    // Release the admission gates once; the cycle no longer holds tick 140.
    for (unsigned i = 0; i < 300; ++i) {
        network.round = round;
        for (unsigned peer = 0; peer < count; ++peer) if (network.open[peer]) {
            const auto result = require(sessions[peer].pump(round));
            departures[peer] += static_cast<unsigned>(result.departure_events.size());
        }
        ++round;
    }
    std::string hash;
    for (unsigned peer = 0; peer < count; ++peer) if (network.open[peer]) {
        expect(adapters[peer]->completed_tick() == 240 && sessions[peer].input_delay() == 8
            && retained[peer] == 1 && departures[peer] == (double_loss ? 2U : 1U),
            "retained timing and departure each apply once through match end");
        if (hash.empty()) hash = adapters[peer]->state_hash();
        expect(hash == adapters[peer]->state_hash(), "retained timing departure survivor hashes agree");
    }
    return hash;
}

std::string triple_tactical_departures(unsigned workers) {
    DepartureGate gate; gate.final_tick = 40;
    // The observer intentionally withholds its next batch for 116 fake ms.
    // Keep that fixture hold below completeness expiry: only peers 0/1/2 leave.
    DepartureMatch match(5, false, false, workers, false, 2, &gate, 4, 1000);
    bool third_before_second_settles{};
    match.network.fault = [](auto from, auto to, auto bytes) {
        FakeDelivery delivery;
        if (from == 1 && to == 4 && bytes.front() == net::departure_ack_kind && bytes[1] == 0)
            delivery.ordered_delay = 20;
        if (from == 2 && to == 4 && bytes.front() == net::departure_proposal_kind && bytes[1] == 1)
            delivery.ordered_delay = 5;
        if (from == 2 && to == 4 && bytes.front() == net::departure_ack_kind && bytes[1] == 1)
            delivery.ordered_delay = 40;
        return delivery;
    };
    match.network.before_send = [&](auto from, auto to, auto bytes) {
        if (from == 3 && to == 4 && bytes.front() == net::departure_proposal_kind && bytes[1] == 2
            && gate.settled.size() == 1) third_before_second_settles = true;
    };
    std::uint64_t round{};
    auto cycle = [&] {
        match.network.round = round;
        for (unsigned peer = 0; peer < 5; ++peer) if (match.network.open[peer]) require(match.sessions[peer].pump(round));
        ++round;
    };
    for (unsigned i = 0; i < 8; ++i) cycle();
    match.network.open[0] = false;
    for (unsigned i = 0; i < 8; ++i) cycle();
    expect(gate.settled.empty(), "first decision remains inside its delayed settle window");
    match.network.open[1] = false;
    for (unsigned i = 0; i < 30; ++i) cycle();
    expect(gate.settled.size() == 1, "observer is waiting on the delayed second-departure stream");
    match.network.open[2] = false;
    for (unsigned i = 0; i < 70; ++i) cycle();
    expect(third_before_second_settles && gate.settled.size() == 3,
        "later target on one stream cannot obstruct another stream's current votes");
    gate.allowed = true;
    for (unsigned i = 0; i < 150; ++i) cycle();
    expect(gate.settled.size() == 3, "delayed healthy survivor never becomes a fourth departure");
    for (const auto peer : {3U, 4U}) {
        expect(match.sessions[peer].simulation().completed_tick() == 40
            && match.sessions[peer].departure_outcomes().size() == 3,
            "three losses each apply once through match end: peer=" + std::to_string(peer)
                + " tick=" + std::to_string(match.sessions[peer].simulation().completed_tick())
                + " outcomes=" + std::to_string(match.sessions[peer].departure_outcomes().size()));
        for (const auto& event : match.sessions[peer].departure_outcomes())
            expect(event.event.key.seat.value >= 1 && event.event.key.seat.value <= 3,
                "only the three physically closed seats depart");
        match.replay(peer);
    }
    expect(match.sessions[3].simulation().record() == match.sessions[4].simulation().record()
        && match.sessions[3].simulation().state_sha256() == match.sessions[4].simulation().state_sha256(),
        "three-loss survivors retain the same replay and hash");
    return match.sessions[4].simulation().state_sha256();
}

std::string maximum_roster_departures(unsigned workers, bool double_loss) {
    constexpr unsigned count = 9;
    const auto warm_tick = double_loss ? 400U : 2050U;
    const auto final_tick = double_loss ? 600U : 2500U;
    const unsigned observer = double_loss ? 7U : 1U;
    FakeNetwork network(count);
    eawr::platform::ThreadWorkerAdapter executor(workers);
    auto initial = setup();
    initial.departure_policy.emplace();
    std::vector<tactical::SeatBinding> roster;
    std::vector<net::PeerSeat> seats;
    for (unsigned peer = 0; peer < count; ++peer) {
        const auto id = peer + 1;
        if (id > 2) {
            initial.players.push_back({id, id, id, tactical::player_flag_commandable});
            auto unit = initial.units.front(); unit.entity_id = id; unit.owner = id;
            unit.position = {units(50 * static_cast<std::int64_t>(id)), {}, {}};
            initial.units.push_back(unit);
        }
        initial.departure_policy->humans.push_back(id);
        roster.push_back({{id}, id, true, false}); seats.push_back({{id}, true});
    }
    std::array<DepartureGate, count> gates;
    std::array<unsigned, count> retained{}, departures{};
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::TacticalSessionAdapter>> adapters;
    std::vector<net::LockstepSession> sessions;
    unsigned deferred_traffic{};
    for (unsigned peer = 0; peer < count; ++peer) {
        gates[peer].allowed = true; gates[peer].final_tick = final_tick;
        transports.push_back(std::make_unique<FakeTransport>(network, peer));
        adapters.push_back(std::make_unique<net::TacticalSessionAdapter>(
            require(tactical::TacticalSession::create(initial, content())), executor, roster));
        net::SessionOptions options;
        options.input_delay = 64; options.final_tick_count = final_tick; options.completeness_timeout_ms = 1000;
        if (!double_loss) {
            options.non_lan = true;
            options.adaptive_timing = net::AdaptiveTiming{0, true, [](auto) { return std::optional<std::uint64_t>{0}; }};
            options.retain_timing_event = [&, peer](auto tick, auto) {
                expect(tick == 2309, "maximum-delay timing keeps its original activation");
                ++retained[peer]; return eawr::core::Result<void>::success();
            };
        }
        sessions.push_back(require(net::LockstepSession::create(*adapters.back(), *transports.back(), seats, options, &gates[peer])));
        sessions.back().enable_departures([&](auto key) {
            return net::encode_tactical_input(key, roster[key.seat.value - 1], {{}, tactical::QuitPayload{}});
        });
    }
    network.fault = [&](auto from, auto to, auto bytes) {
        FakeDelivery delivery;
        if (!double_loss && from == 0 && to == observer && bytes.front() == net::timing_ready_kind)
            delivery.ordered_delay = 80;
        if (double_loss && from == 1 && to == observer && bytes.front() == net::departure_ack_kind && bytes[1] == 0)
            delivery.ordered_delay = 80;
        return delivery;
    };
    network.before_send = [&](auto, auto to, auto bytes) {
        if (to == observer && bytes.front() >= net::departure_proposal_kind && bytes.front() <= net::departure_ack_kind
            && bytes[1] == (double_loss ? 1U : 0U)
            && (double_loss ? gates[observer].settled.empty() : retained[observer] == 0)) ++deferred_traffic;
    };
    std::uint64_t round{};
    auto cycle = [&](bool hold) {
        network.round = round;
        for (unsigned peer = 0; peer < count; ++peer) if (network.open[peer]) {
            if (hold && adapters[peer]->completed_tick() == warm_tick) gates[peer].allowed = false;
            const auto result = require(sessions[peer].pump(round));
            departures[peer] += static_cast<unsigned>(result.departure_events.size());
        }
        ++round;
    };
    for (unsigned i = 0; i < warm_tick + 10; ++i) cycle(true);
    for (unsigned peer = 0; peer < count; ++peer)
        expect(adapters[peer]->completed_tick() == warm_tick, "maximum-roster case reaches a full retained recovery window");
    if (!double_loss) expect(retained[observer] == 0 && retained[2] == 1, "nine-peer observer waits on ordered timing ready");
    network.open[0] = false;
    if (double_loss) {
        for (unsigned i = 0; i < 8; ++i) cycle(true);
        network.open[1] = false;
    }
    for (unsigned i = 0; i < 120; ++i) cycle(true);
    constexpr auto horizon = net::sealed_input_horizon(64);
    static_assert(horizon == 130);
    expect(net::departure_frame_limit(horizon, count - 1) == 2112
        && net::departure_frame_limit(horizon, count - 2) == 1848,
        "nine-peer departure cap covers eight remotes and differs from eight-peer scaling");
    expect(deferred_traffic > 1024, "honest recovery traffic exceeds the minimum departure frame cap");
    for (unsigned peer = 0; peer < count; ++peer) if (network.open[peer]) {
        expect(gates[peer].settled.size() == (double_loss ? 2U : 1U), "honest maximum-roster votes settle without a resource refusal");
        gates[peer].allowed = true;
    }
    for (unsigned i = 0; i < 1000; ++i) cycle(false);
    std::string hash;
    for (unsigned peer = 0; peer < count; ++peer) if (network.open[peer]) {
        expect(adapters[peer]->completed_tick() == final_tick && departures[peer] == (double_loss ? 2U : 1U)
            && sessions[peer].input_delay() == (double_loss ? 64U : 4U), "maximum-roster survivors apply each decision through match end");
        if (hash.empty()) hash = adapters[peer]->state_hash();
        expect(hash == adapters[peer]->state_hash(), "maximum-roster survivor hashes agree");
    }
    return hash;
}

void agreed_departures() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        DepartureMatch quit(2, false, false, workers);
        const auto stamp = require(quit.sessions[0].submit({{}, tactical::QuitPayload{}}));
        std::array<std::optional<net::DepartureOutcome>, 2> seen;
        std::array<std::string, 2> departure_hashes;
        for (std::uint64_t round = 0; round < 160; ++round) {
            quit.network.round = round;
            for (unsigned peer = 0; peer < 2; ++peer) {
                const auto pumped = require(quit.sessions[peer].pump(round));
                if (!pumped.departure_outcomes.empty()) {
                    seen[peer] = pumped.departure_outcomes.front();
                    departure_hashes[peer] = quit.sessions[peer].simulation().state_sha256();
                }
            }
        }
        expect(seen[0] && seen[1] && seen[0]->event.key == stamp && seen[1]->event.key == stamp,
            "sealed quit executes at same tick on both peers");
        if (seen[0] && seen[1]) expect(seen[0]->state == net::DepartureState::match_ended
            && seen[1]->state == net::DepartureState::won_by_departure, "local quitter and opponent get typed departure results");
        expect(!departure_hashes[0].empty() && departure_hashes[0] == departure_hashes[1], "quit peers agree on the completed departure frame");
        expect(!quit.sessions[0].failure(), "non-monitored quitter stays inert beyond completeness expiry");
        quit.replay(1);
    }
    for (const bool timeout : {false, true}) {
        DepartureMatch match(2);
        for (std::uint64_t round = 0; round < 12; ++round) {
            match.network.round = round;
            for (auto& session : match.sessions) require(session.pump(round));
        }
        if (!timeout) match.network.open[0] = false;
        bool lost{};
        for (std::uint64_t round = 12; round < 160 && !lost; ++round) {
            match.network.round = round;
            const auto result = match.sessions[1].pump(round);
            if (!result) { lost = true; expect(result.error().code == net::session_errors::stalled, "timeout and lone-survivor disconnect do not imply Quit"); }
        }
        expect(lost && match.sessions[1].departure_outcomes().empty() && !match.sessions[1].simulation().outcome(),
            "unconfirmed two-peer loss ends without a winner");
        match.replay(1);
    }
    for (const bool human_ally : {false, true}) for (const bool ai_ally : {false, true}) {
        DepartureMatch match(3, human_ally, ai_ally, 4);
        match.network.fault = [](auto from, auto to, auto frame) {
            FakeDelivery delivery;
            delivery.drop = from == 0 && to == 2 && frame.front() == net::sealed_batch_kind && batch_tick(frame) >= 3;
            return delivery;
        };
        require(match.sessions[0].submit({{1}, tactical::StopPayload{}}));
        for (std::uint64_t round = 0; round < 20; ++round) {
            match.network.round = round;
            for (auto& session : match.sessions) require(session.pump(round));
        }
        expect(match.sessions[1].simulation().completed_tick() > match.sessions[2].simulation().completed_tick(), "survivors saw different departed frontiers");
        match.network.open[0] = false;
        std::array<std::optional<net::DepartureOutcome>, 2> seen;
        for (std::uint64_t round = 20; round < 100; ++round) {
            match.network.round = round;
            for (unsigned peer = 1; peer < 3; ++peer) {
                const auto pumped = require(match.sessions[peer].pump(round));
                if (!pumped.departure_outcomes.empty()) seen[peer - 1] = pumped.departure_outcomes.front();
            }
            if (match.sessions[1].simulation().completed_tick() == 40 && match.sessions[2].simulation().completed_tick() == 40) break;
        }
        expect(seen[0] && seen[1] && seen[0]->event == seen[1]->event, "three-peer disconnect has one agreed tick and reason");
        expect(match.sessions[1].simulation().record() == match.sessions[2].simulation().record(), "recovered commands and synthetic input agree in replay");
        expect(match.sessions[1].simulation().state_sha256() == match.sessions[2].simulation().state_sha256(), "three-peer recovered takeover state hashes agree");
        const auto expected_owner = human_ally ? 3U : ai_ally ? 4U : 1U;
        expect(match.sessions[1].simulation().units().front().owner == expected_owner, "MP-37 human before AI before basic own-faction takeover");
        expect(!match.sessions[1].simulation().outcome(), "MP-38 two active controllable players continue");
        if (seen[0]) expect(seen[0]->state == (human_ally ? net::DepartureState::continues : net::DepartureState::continues_with_ai),
            "typed continuation distinguishes human and AI recipients");
        if (!human_ally && !ai_ally) {
            expect(match.attached[1].size() == 1 && match.attached[1] == match.attached[2], "same AI controller attached once at agreed tick on survivors");
            expect(match.sessions[1].simulation().record().commands.size() > 2, "trusted takeover AI progresses through engine step hook");
        } else expect(match.attached[1].empty(), "allied recipient avoids installing redundant basic controller");
        match.replay(1);
    }
}
}
