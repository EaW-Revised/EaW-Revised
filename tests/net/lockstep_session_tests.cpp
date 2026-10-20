#include "../support/check.hpp"
#include "lockstep_session_fixtures.hpp"
#include "fake_transport.hpp"
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/net/session_monitor.hpp"
#include "eawr/net/transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

std::string agreement(std::uint32_t peers, std::uint64_t delay, std::size_t workers, const std::string& fault = {},
    std::optional<eawr::testing::WanProfile> wan = {});
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
class OpaqueAdapter final : public net::SessionAdapter {
public:
    std::uint64_t tick{};
    std::vector<std::uint8_t> history;
    std::vector<sim::SeatCommandKey> installed;
    std::vector<net::SeatInput> executed;
    bool recognize_departures{};
    std::uint64_t completed_tick() const override { return tick; }
    std::string state_hash() const override { return eawr::core::sha256_hex(canonical_state()); }
    std::vector<std::uint8_t> canonical_state() const override {
        auto bytes = history;
        bytes.push_back(static_cast<std::uint8_t>(tick));
        return bytes;
    }
    eawr::core::Result<void> validate(const net::SeatInput& input) const override {
        if (input.bytes.size() == 1 && (input.bytes.front() == input.key.seat.value
            || (recognize_departures && input.bytes.front() == 0)))
            return eawr::core::Result<void>::success();
        eawr::core::Diagnostic error;
        error.code = net::session_errors::admission;
        error.message = "opaque provider refused input";
        return eawr::core::Result<void>::failure(std::move(error));
    }
    eawr::core::Result<void> install(std::span<const net::SeatInput> commands) override {
        installed.clear();
        for (const auto& input : commands) {
            expect(installed.empty() || installed.back() < input.key, "core installs canonical seat order");
            installed.push_back(input.key);
            executed.push_back(input);
            history.insert(history.end(), input.bytes.begin(), input.bytes.end());
        }
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<std::vector<std::uint8_t>> canonical_input() const override {
        return eawr::core::Result<std::vector<std::uint8_t>>::success({});
    }
    eawr::core::Result<net::CompletedTick> step() override {
        ++tick;
        return eawr::core::Result<net::CompletedTick>::success({tick, state_hash()});
    }
    bool is_departure(const net::SeatInput& input) const override {
        return recognize_departures && input.bytes.size() == 1 && input.bytes.front() == 0;
    }
};

void chained_departures() {
    FakeNetwork network(4);
    std::array<OpaqueAdapter, 4> adapters;
    std::array<DepartureGate, 4> gates;
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<net::LockstepSession> sessions;
    const std::vector<net::PeerSeat> roster{{{1}, true}, {{2}, true}, {{3}, true}, {{4}, true}};
    for (unsigned peer = 0; peer < 4; ++peer) {
        adapters[peer].recognize_departures = true;
        gates[peer].final_tick = 30;
        net::SessionOptions options; options.final_tick_count = 30; options.completeness_timeout_ms = 100;
        transports.push_back(std::make_unique<FakeTransport>(network, peer));
        sessions.push_back(require(net::LockstepSession::create(adapters[peer], *transports.back(), roster, options, &gates[peer])));
        sessions.back().enable_departures([](auto) { return eawr::core::Result<std::vector<std::uint8_t>>::success({0}); });
    }
    std::uint64_t round{};
    auto cycle = [&] {
        network.round = round;
        for (unsigned peer = 0; peer < 4; ++peer)
            if (network.open[peer]) require(sessions[peer].pump(round));
        ++round;
    };
    for (unsigned i = 0; i < 4; ++i) cycle();
    network.open[0] = false;
    for (unsigned i = 0; i < 8; ++i) cycle();
    for (unsigned peer = 1; peer < 4; ++peer)
        expect(gates[peer].settled.size() == 1 && adapters[peer].tick == 0 && adapters[peer].executed.empty(),
            "first departure settles while its cutoff remains unapplied");
    network.open[1] = false;
    for (unsigned i = 0; i < 8; ++i) cycle();
    for (unsigned peer = 2; peer < 4; ++peer) {
        expect(gates[peer].settled.size() == 2 && gates[peer].settled[0].first == 0 && gates[peer].settled[1].first == 1,
            "second departure settles without the agreed but unapplied voter");
        gates[peer].allowed = true;
    }
    for (unsigned i = 0; i < 150; ++i) cycle();
    expect(adapters[2].tick == 30 && adapters[3].tick == 30 && adapters[2].state_hash() == adapters[3].state_hash(),
        "chained departures complete with equal survivor hashes beyond the agreement timeout");
    expect(adapters[2].executed == adapters[3].executed && adapters[2].executed.size() == 2
        && adapters[2].executed[0].key.seat == sim::SeatId{1} && adapters[2].executed[1].key.seat == sim::SeatId{2},
        "both synthetic departures execute once in canonical order");
}

void target_timing_controls() {
    for (const auto kind : {net::timing_proposal_kind, net::timing_ready_kind}) for (const bool settled : {false, true}) {
        FakeNetwork network(3);
        std::array<OpaqueAdapter, 3> adapters;
        std::array<DepartureGate, 3> gates;
        std::vector<std::unique_ptr<FakeTransport>> transports;
        std::vector<net::LockstepSession> sessions;
        const std::vector<net::PeerSeat> roster{{{1}, true}, {{2}, true}, {{3}, true}};
        for (unsigned peer = 0; peer < 3; ++peer) {
            adapters[peer].recognize_departures = true;
            gates[peer].final_tick = 40;
            net::SessionOptions options; options.input_delay = 4; options.final_tick_count = 40;
            options.non_lan = true; options.adaptive_timing = net::AdaptiveTiming{0, true, {}};
            transports.push_back(std::make_unique<FakeTransport>(network, peer));
            sessions.push_back(require(net::LockstepSession::create(adapters[peer], *transports.back(), roster, options, &gates[peer])));
            sessions.back().enable_departures([](auto) { return eawr::core::Result<std::vector<std::uint8_t>>::success({0}); });
        }
        std::uint64_t round{};
        auto cycle = [&] {
            network.round = round;
            for (unsigned peer = 0; peer < 3; ++peer)
                if (network.open[peer]) require(sessions[peer].pump(round));
            ++round;
        };
        for (unsigned i = 0; i < 4; ++i) cycle();
        network.open[0] = false;
        cycle(); // Both survivors propose; neither has received the other's vote.
        if (settled) for (unsigned i = 0; i < 8; ++i) cycle();
        expect(gates[2].settled.size() == (settled ? 1U : 0U), "target control exercises both sides of local settle");
        std::array<std::uint8_t, 17> control{};
        control[0] = kind; control[1] = 25; control[9] = 8;
        network.deliver(0, 2, control);
        for (unsigned i = 0; i < 8; ++i) cycle();
        expect(gates[2].settled.size() == 1 && adapters[2].tick == 0 && sessions[2].input_delay() == 4,
            "target timing control is dropped independently of acknowledgement drain order");
        gates[1].allowed = gates[2].allowed = true;
        for (unsigned i = 0; i < 80; ++i) cycle();
        expect(adapters[1].tick == 40 && adapters[2].tick == 40 && adapters[1].state_hash() == adapters[2].state_hash()
            && sessions[2].input_delay() == 4, "dropped target timing cannot introduce an unvoted delay change");
    }
}

void final_tick_departure_ack() {
    FakeNetwork network(3);
    std::array<OpaqueAdapter, 3> adapters;
    std::array<DepartureGate, 3> gates;
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<net::LockstepSession> sessions;
    const std::vector<net::PeerSeat> roster{{{1}, true}, {{2}, true}, {{3}, true}};
    for (unsigned peer = 0; peer < 3; ++peer) {
        gates[peer].allowed = true; gates[peer].final_tick = 1;
        net::SessionOptions options; options.final_tick_count = 1;
        transports.push_back(std::make_unique<FakeTransport>(network, peer));
        sessions.push_back(require(net::LockstepSession::create(adapters[peer], *transports.back(), roster, options, &gates[peer])));
        sessions.back().enable_departures([](auto) { return eawr::core::Result<std::vector<std::uint8_t>>::success({0}); });
    }
    for (std::uint64_t round = 0; round < 4; ++round) {
        network.round = round;
        for (auto& session : sessions) require(session.pump(round));
    }
    expect(adapters[2].tick == 1, "late departure vote arrives after the final simulation tick");
    unsigned acknowledgements{};
    network.before_send = [&](auto, auto, auto frame) { if (frame.front() == net::departure_ack_kind) ++acknowledgements; };
    const std::array<std::uint8_t, 14> proposal{net::departure_proposal_kind, 0, 0, 0, 0, 1};
    const std::array<std::uint8_t, 5> ready{net::departure_ready_kind};
    const std::array<std::uint8_t, 13> ack{net::departure_ack_kind};
    network.deliver(1, 2, proposal); network.deliver(1, 2, ready); network.deliver(1, 2, ack);
    network.round = 4;
    require(sessions[2].pump(4));
    require(sessions[2].pump(1000));
    expect(adapters[2].tick == 1 && gates[2].settled.empty() && acknowledgements == 0,
        "receive-path acknowledgements cannot finalize a departure beyond the final-tick return");
}

void deferred_departure_refusals() {
    for (const auto fault : {0U, 1U, 2U, 3U}) {
        FakeNetwork network(4);
        std::array<OpaqueAdapter, 4> adapters;
        std::array<DepartureGate, 4> gates;
        std::vector<std::unique_ptr<FakeTransport>> transports;
        std::vector<net::LockstepSession> sessions;
        const std::vector<net::PeerSeat> seats{{{1}, true}, {{2}, true}, {{3}, true}, {{4}, true}};
        for (unsigned peer = 0; peer < 4; ++peer) {
            adapters[peer].recognize_departures = true; gates[peer].final_tick = 30;
            transports.push_back(std::make_unique<FakeTransport>(network, peer));
            net::SessionOptions options; options.final_tick_count = 30; options.completeness_timeout_ms = 100;
            sessions.push_back(require(net::LockstepSession::create(adapters[peer], *transports.back(), seats, options, &gates[peer])));
            sessions.back().enable_departures([](auto) { return eawr::core::Result<std::vector<std::uint8_t>>::success({0}); });
        }
        network.fault = [fault](auto from, auto to, auto bytes) {
            FakeDelivery delivery;
            delivery.drop = from == 1 && to == 3 && bytes.front() == net::departure_ack_kind;
            if (fault == 0 && from == 2 && to == 3 && bytes.front() == net::departure_ack_kind) delivery.drop = true;
            return delivery;
        };
        for (unsigned round = 0; round < 16; ++round) {
            network.round = round;
            if (round == 4) network.open[0] = false;
            for (unsigned peer = 0; peer < 4; ++peer) if (network.open[peer]) require(sessions[peer].pump(round));
        }
        const auto original = adapters[3].state_hash();
        std::vector<std::uint8_t> frame{net::departure_proposal_kind, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
        if (fault == 2) { frame.resize(net::max_batch_bytes / 2 + 1, 0); frame[0] = net::departure_recovery_kind; }
        const auto count = fault == 0 ? 1U : fault == 1 ? 1025U : fault == 2 ? 2U : 0U;
        for (unsigned index = 0; index < count; ++index) network.deliver(2, 3, frame);
        if (fault == 3) network.open[1] = false;
        network.round = 17;
        auto result = sessions[3].pump(17);
        if (fault == 1) {
            for (unsigned round = 18; round < 22 && result; ++round) {
                network.round = round; result = sessions[3].pump(round);
            }
        }
        if (fault == 3) {
            expect(static_cast<bool>(result), "second loss waits for the original voter proof");
            const auto expired = sessions[3].pump(116);
            expect(!expired && expired.error().code == net::departure_errors::timeout,
                "a voter lost without its first acknowledgement causes bounded agreement refusal");
        } else {
            expect(!result && result.error().code == (fault == 0 ? net::departure_errors::agreement : net::session_errors::resource_limit),
                "deferred controls enforce sender proof, horizon frame budget and byte budget");
        }
        expect(adapters[3].tick == 0 && adapters[3].state_hash() == original, "deferred refusal preserves the simulation prefix");
    }
}

void adaptive_settled_departure(bool delayed_host_ack, bool delayed_other_ack) {
    const std::uint32_t count = delayed_other_ack ? 4U : 3U;
    FakeNetwork network(count);
    std::vector<OpaqueAdapter> adapters(count);
    std::vector<std::unique_ptr<FakeTransport>> transports;
    Monitor monitor;
    monitor.allowed = true;
    monitor.final_tick = 240;
    std::vector<net::LockstepSession> sessions;
    std::vector<std::vector<std::string>> hashes(count);
    std::vector<std::uint64_t> submitted(count, 1000);
    std::vector<unsigned> samples(count), departures(count);
    std::vector<std::uint64_t> cutoffs;
    std::vector<net::PeerSeat> roster;
    for (net::PeerIndex peer = 0; peer < count; ++peer) roster.push_back({sim::SeatId{peer + 1}, true});
    for (net::PeerIndex peer = 0; peer < count; ++peer) {
        adapters[peer].recognize_departures = true;
        net::SessionOptions options;
        options.input_delay = 4;
        options.final_tick_count = 240;
        options.completeness_timeout_ms = 10'000;
        options.non_lan = true;
        options.adaptive_timing = net::AdaptiveTiming{0, true, [&, peer](auto remote) {
            expect(peer == 0, "settled-departure policy samples only on the host");
            ++samples[remote];
            return std::optional<std::uint64_t>{remote == 2 ? 5000U : 330U};
        }};
        transports.push_back(std::make_unique<FakeTransport>(network, peer));
        sessions.push_back(require(net::LockstepSession::create(adapters[peer], *transports.back(), roster, options,
            peer == 0 ? &monitor : nullptr)));
        sessions.back().enable_departures([](auto) { return eawr::core::Result<std::vector<std::uint8_t>>::success({0}); });
    }
    network.before_send = [&](auto, auto, auto bytes) {
        if (bytes.front() != net::departure_ack_kind) return;
        std::uint64_t cutoff{};
        for (unsigned i = 0; i < 8; ++i) cutoff |= static_cast<std::uint64_t>(bytes[5 + i]) << (8 * i);
        cutoffs.push_back(cutoff);
    };
    network.fault = [delayed_host_ack, delayed_other_ack](auto from, auto to, auto bytes) {
        FakeDelivery delivery;
        if (to == 1 && bytes.front() == net::departure_ack_kind) {
            if (from == 0 && delayed_host_ack) delivery.ordered_delay = 12;
            if (from == 3 && delayed_other_ack) delivery.ordered_delay = 20;
        }
        return delivery;
    };
    std::uint64_t round{};
    auto cycle = [&](bool include_target) {
        network.round = round;
        for (net::PeerIndex peer = 0; peer < count; ++peer) {
            if (!include_target && peer == 2) continue;
            const auto tick = adapters[peer].tick;
            if (tick < 210 && submitted[peer] != tick) {
                require(sessions[peer].submit([](auto key) {
                    return eawr::core::Result<std::vector<std::uint8_t>>::success({static_cast<std::uint8_t>(key.seat.value)});
                }));
                submitted[peer] = tick;
            }
            const auto pumped = require(sessions[peer].pump(round));
            if (pumped.completed) hashes[peer].push_back(pumped.completed->state_sha256);
            for (const auto& event : pumped.departure_events) {
                expect(event.key.tick == 133 && event.key.seat == sim::SeatId{3}, "settled departure preserves its cutoff");
                ++departures[peer];
            }
        }
        ++round;
    };
    while (round < 300 && adapters[0].tick < 123) cycle(true);
    expect(adapters[0].tick == 123, "settled-departure case reaches pre-cadence tick");
    monitor.allowed = false;
    // Seal the host's window while holding its execution; other peers reach 128
    // and seal through 132, the greatest batch reachable at the host's tick 123.
    while (round < 350 && (adapters[1].tick < 128 || adapters[2].tick < 128)) cycle(true);
    cycle(true);
    network.open[2] = false;
    for (unsigned i = 0; i < 8; ++i) cycle(false);
    expect(adapters[0].tick == 123 && cutoffs.size() == (count - 1) * (count - 2)
        && std::all_of(cutoffs.begin(), cutoffs.end(), [](auto cutoff) { return cutoff == 133; })
        && departures[0] == 0, "departure settles above the next cadence boundary before applying");
    if (!delayed_host_ack && !delayed_other_ack) {
        std::array<std::uint8_t, 17> late{};
        late[0] = net::timing_proposal_kind; late[1] = 165; late[9] = 8;
        network.deliver(2, 0, late);
        late[0] = net::timing_ready_kind;
        network.deliver(2, 1, late);
    }
    monitor.allowed = true;
    while (round < 1000 && std::any_of(adapters.begin(), adapters.end(), [&](const auto& adapter) {
        return &adapter != &adapters[2] && adapter.tick < 240;
    })) cycle(false);
    expect(samples[1] > 0 && samples[2] == 0, "agreed-but-unapplied departure contributes no RTT sample");
    for (net::PeerIndex peer = 0; peer < count; ++peer) {
        if (peer == 2) continue;
        expect(adapters[peer].tick == 240 && sessions[peer].input_delay() == 8,
            "survivors continue through agreed timing and departure without the removed voter");
        expect(departures[peer] == 1, "settled departure applies exactly once on every survivor");
        expect(hashes[0] == hashes[peer] && adapters[0].executed == adapters[peer].executed,
            "settled departure and timing preserve every survivor hash and accepted command");
    }
    expect(adapters[0].executed.size() == (count - 1) * 210 + 130,
        "recovery preserves all in-flight target commands and the synthetic quit");
}

class StatsTransport final : public net::MessageTransport {
public:
    StatsTransport(FakeNetwork& network, net::PeerIndex peer) : delegate_(network, peer) {}
    eawr::core::Result<void> send(net::PeerIndex peer, std::span<const std::uint8_t> bytes) override { return delegate_.send(peer, bytes); }
    std::optional<net::Received> poll() override { return delegate_.poll(); }
    net::LinkState link(net::PeerIndex peer) const override { return delegate_.link(peer); }
    net::PeerIndex self() const override { return delegate_.self(); }
    std::uint32_t peer_count() const override { return delegate_.peer_count(); }
    std::optional<net::LinkStats> link_stats(net::PeerIndex) const override { ++reads; return stats; }
    std::optional<net::LinkStats> stats{net::LinkStats{std::chrono::microseconds{330'000}, std::chrono::microseconds{330'000}, {}, 1, 0.0}};
    mutable unsigned reads{};
private:
    FakeTransport delegate_;
};

struct AdaptiveOpaque {
    FakeNetwork network;
    std::vector<OpaqueAdapter> adapters;
    std::vector<StatsTransport> transports;
    std::vector<net::LockstepSession> sessions;
    explicit AdaptiveOpaque(std::uint64_t timeout = 1000, bool override_provider = true,
        std::function<eawr::core::Result<void>(std::uint64_t, std::span<const std::uint8_t>)> retain = {},
        std::uint64_t initial_delay = 4, unsigned peers = 2)
        : network(peers), adapters(peers) {
        net::SessionOptions options;
        options.input_delay = initial_delay;
        options.final_tick_count = 500;
        options.non_lan = true;
        options.retain_timing_event = std::move(retain);
        options.completeness_timeout_ms = timeout;
        options.adaptive_timing = net::AdaptiveTiming{0, true, [](auto) { return std::optional<std::uint64_t>{330}; }};
        if (!override_provider) options.adaptive_timing->rtt_ms = {};
        transports.reserve(peers);
        std::vector<net::PeerSeat> roster;
        for (unsigned peer = 0; peer < peers; ++peer) {
            transports.emplace_back(network, peer);
            roster.push_back({sim::SeatId{peer + 1}, true});
        }
        for (unsigned peer = 0; peer < peers; ++peer)
            sessions.push_back(require(net::LockstepSession::create(adapters[peer], transports[peer],
                roster, options)));
    }
};

std::vector<std::uint8_t> timing_frame(std::uint8_t kind, std::uint64_t tick, std::uint64_t delay) {
    std::vector<std::uint8_t> bytes{kind};
    for (const auto value : {tick, delay}) for (unsigned i = 0; i < 8; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    return bytes;
}

void adaptive_transition_proposal() {
    FakeNetwork network(2);
    std::array<OpaqueAdapter, 2> adapters;
    std::array<FakeTransport, 2> transports{{FakeTransport(network, 0), FakeTransport(network, 1)}};
    Monitor gate;
    gate.allowed = true;
    gate.final_tick = 900;
    std::vector<net::LockstepSession> sessions;
    std::array<std::vector<std::string>, 2> hashes;
    std::array<std::vector<std::pair<std::uint64_t, std::uint64_t>>, 2> changes;
    net::SessionOptions options;
    options.input_delay = 20;
    options.final_tick_count = 900;
    options.non_lan = true;
    options.adaptive_timing = net::AdaptiveTiming{0, true, [&](auto) {
        return std::optional<std::uint64_t>{adapters[0].tick < 700 ? 0U : 330U};
    }};
    for (unsigned peer = 0; peer < 2; ++peer)
        sessions.push_back(require(net::LockstepSession::create(adapters[peer], transports[peer],
            {{sim::SeatId{1}, true}, {sim::SeatId{2}, true}}, options, peer == 1 ? &gate : nullptr)));
    std::uint64_t round{};
    auto cycle = [&] {
        network.round = round;
        for (unsigned peer = 0; peer < 2; ++peer) {
            const auto tick = adapters[peer].tick;
            const auto delay = sessions[peer].input_delay();
            const auto result = require(sessions[peer].pump(round));
            if (result.completed) hashes[peer].push_back(result.completed->state_sha256);
            if (sessions[peer].input_delay() != delay) changes[peer].push_back({tick, sessions[peer].input_delay()});
        }
        ++round;
    };
    while (round < 1000 && adapters[1].tick < 764) cycle();
    expect(adapters[0].tick == 764 && adapters[1].tick == 764, "transition case reaches the retained old-delay window");
    gate.allowed = false;
    while (round < 1050 && adapters[0].tick < 769) cycle();
    cycle(); // Host's new proposal arrives while receiver is still below retain_until=767.
    expect(adapters[1].tick == 764 && sessions[1].input_horizon() == net::sealed_input_horizon(20),
        "lagging receiver accepts a later activation while retaining the prior envelope");
    gate.allowed = true;
    while (round < 1400 && (adapters[0].tick < 900 || adapters[1].tick < 900)) cycle();
    expect(adapters[0].tick == 900 && adapters[1].tick == 900 && hashes[0] == hashes[1],
        "successive timing events agree despite retention-boundary receive skew");
    expect(changes[0] == std::vector<std::pair<std::uint64_t, std::uint64_t>>{{725, 4}, {805, 8}}
        && changes[1] == changes[0], "later event activates beyond the prior transition on both peers");
}

void adaptive_wire_and_gate() {
    {
        FakeNetwork network(3);
        OpaqueAdapter adapter;
        FakeTransport transport(network, 1);
        net::SessionOptions options;
        options.input_delay = 4;
        options.final_tick_count = 500;
        options.non_lan = true;
        options.adaptive_timing = net::AdaptiveTiming{};
        auto session = require(net::LockstepSession::create(adapter, transport,
            {{sim::SeatId{1}, true}, {sim::SeatId{2}, true}, {sim::SeatId{3}, true}}, options));
        session.enable_departures([](auto key) {
            return eawr::core::Result<std::vector<std::uint8_t>>::success({static_cast<std::uint8_t>(key.seat.value)});
        });
        network.open[2] = false;
        require(session.pump(0)); // Peer 0's vote is absent, so departure remains open.
        network.deliver(0, 1, timing_frame(net::timing_proposal_kind, 40, 8));
        network.round = 1;
        const auto result = session.pump(1);
        expect(!result && result.error().code == net::timing_errors::agreement
            && adapter.tick == 0 && session.input_delay() == 4, "timing proposal during open three-peer departure fails closed");
    }
    for (const auto kind : {net::timing_proposal_kind, net::timing_ready_kind}) {
        AdaptiveOpaque match;
        std::vector<std::uint8_t> oversized(net::max_batch_bytes + 6);
        oversized.front() = kind;
        match.network.deliver(1, 0, oversized);
        match.network.round = 1;
        const auto result = match.sessions[0].pump(1);
        expect(!result && result.error().code == net::timing_errors::malformed,
            "oversized timing envelope uses reserved timing diagnostic");
    }
    for (const bool departures : {false, true}) for (const bool ready : {false, true}) {
        AdaptiveOpaque match(1000, true, {}, 4, departures && ready ? 3U : 2U);
        if (departures) for (unsigned peer = 0; peer < match.sessions.size(); peer += 2) {
            match.adapters[peer].recognize_departures = ready;
            match.sessions[peer].enable_departures([ready](auto key) {
                return eawr::core::Result<std::vector<std::uint8_t>>::success({static_cast<std::uint8_t>(ready ? 0 : key.seat.value)});
            });
        }
        match.network.fault = [ready](auto from, auto to, auto bytes) {
            FakeDelivery delivery;
            delivery.drop = !ready && from == 1 && to == 0 && bytes.front() == net::timing_ready_kind;
            return delivery;
        };
        std::uint64_t round{};
        for (; round < 200 && match.adapters[0].tick < 140; ++round) {
            match.network.round = round;
            for (auto& session : match.sessions) require(session.pump(round));
        }
        expect(match.adapters[0].tick == 140, "failed-voter case reaches pending timing agreement");
        match.network.open[1] = false;
        match.network.round = round;
        const auto result = match.sessions[0].pump(round);
        if (departures && ready) {
            expect(static_cast<bool>(result), "retained timing opens a vote between two survivors");
            std::array<bool, 3> departed{};
            for (unsigned attempt = 0; attempt < 32 && !(departed[0] && departed[2]); ++attempt) {
                match.network.round = ++round;
                for (const auto peer : {0U, 2U}) {
                    const auto pumped = require(match.sessions[peer].pump(round));
                    for (const auto& event : pumped.departure_events)
                        if (event.key.seat == sim::SeatId{2}) departed[peer] = true;
                }
            }
            expect(departed[0] && departed[2] && match.sessions[0].input_delay() == 4
                && match.sessions[2].input_delay() == 4, "retained timing permits the agreed voter departure");
            match.network.round = ++round;
            for (const auto peer : {0U, 2U})
                expect(static_cast<bool>(match.sessions[peer].pump(round)), "retained event survives subsequent departure pumping");
            continue;
        }
        if (departures) {
            expect(result && match.adapters[0].tick == 140 && match.sessions[0].input_delay() == 4,
                "departure waits at the unretained timing boundary without stepping");
            const auto expired = match.sessions[0].pump(round + 1000);
            expect(!expired && expired.error().code == net::timing_errors::timeout,
                "missing timing proof expires rather than inventing retention");
            const auto repeated = match.sessions[0].pump(round + 1001);
            expect(!repeated && repeated.error().code == net::timing_errors::timeout, "timing expiry is latched");
            continue;
        }
        expect(!result && result.error().code == net::timing_errors::agreement,
            "closed timing voter fails before and after readiness, including departure overlap");
        expect(match.adapters[0].tick == 140 && match.sessions[0].input_delay() == 4,
            "failed timing voter never advances or applies the proposed delay");
        const auto repeated = match.sessions[0].pump(round + 1);
        expect(!repeated && repeated.error().code == net::timing_errors::agreement, "failed timing voter diagnostic is latched");
    }
    {
        AdaptiveOpaque match(1000, false, {}, 2);
        match.transports[0].stats = net::LinkStats{{}, {}, {}, 1, 0.0};
        std::array<std::optional<std::uint64_t>, 2> applied;
        for (std::uint64_t round = 0; round < 140; ++round) {
            match.network.round = round;
            for (unsigned peer = 0; peer < 2; ++peer) {
                const auto tick = match.adapters[peer].tick;
                const auto delay = match.sessions[peer].input_delay();
                require(match.sessions[peer].pump(round));
                if (delay != match.sessions[peer].input_delay()) applied[peer] = tick;
            }
        }
        expect(applied[0] == 85 && applied[1] == applied[0] && match.sessions[0].input_delay() == 4,
            "initial platform R=1 evaluates at 64 and applies low-ping R=2/A=4 at the agreed future tick");
    }
    {
        AdaptiveOpaque match;
        net::SessionOptions lan;
        lan.adaptive_timing = net::AdaptiveTiming{};
        const auto result = net::LockstepSession::create(match.adapters[0], match.transports[0],
            {{sim::SeatId{1}, true}, {sim::SeatId{2}, true}}, lan);
        expect(!result && result.error().code == net::timing_errors::options, "LAN policy excludes adaptive timing");
        const auto increase = timing_frame(net::timing_proposal_kind, 165, 8);
        expect(!match.sessions[0].replay_timing_control(increase), "replay cannot apply a future timing event early");
        match.adapters[0].tick = 165;
        require(match.sessions[0].replay_timing_control(increase));
        require(match.sessions[0].replay_timing_control(increase));
        expect(match.sessions[0].input_delay() == 8, "immutable catch-up control applies once at original tick");
        expect(!match.sessions[0].replay_timing_control(timing_frame(net::timing_proposal_kind, 165, 6)),
            "catch-up rejects contradictory control at the same tick");
        match.adapters[0].tick = 293;
        require(match.sessions[0].replay_timing_control(timing_frame(net::timing_proposal_kind, 293, 4)));
        expect(match.sessions[0].input_delay() == 4 && match.sessions[0].input_horizon() == net::sealed_input_horizon(8),
            "catch-up decrease preserves the immutable transition horizon");
        match.adapters[0].tick = 311;
        expect(match.sessions[0].input_horizon() == net::sealed_input_horizon(4), "catch-up transition drains at original boundary");
    }
    {
        AdaptiveOpaque match(1000, false);
        unsigned proposals{};
        match.network.before_send = [&](auto, auto, auto bytes) { if (bytes.front() == net::timing_proposal_kind) ++proposals; };
        match.adapters[0].tick = 165;
        require(match.sessions[0].replay_timing_control(timing_frame(net::timing_proposal_kind, 165, 8)));
        match.transports[0].stats = net::LinkStats{{}, {}, {}, 1, 0.0};
        match.adapters[0].tick = 200;
        require(match.sessions[0].pump(1));
        expect(proposals == 0, "catch-up does not invent an off-cadence host policy pass");
        match.adapters[0].tick = 256;
        require(match.sessions[0].pump(2));
        expect(proposals == 1, "catch-up host resumes on the original global policy boundary");
    }
    {
        unsigned retained{};
        AdaptiveOpaque match(1000, true, [&](auto, auto) {
            ++retained;
            eawr::core::Diagnostic error;
            error.code = "EAWR-TEST-RETENTION";
            error.message = "synthetic retention failure";
            return eawr::core::Result<void>::failure(std::move(error));
        });
        bool failed{};
        for (std::uint64_t round = 0; round < 200 && !failed; ++round) {
            match.network.round = round;
            for (auto& session : match.sessions) {
                const auto result = session.pump(round);
                if (!result) {
                    expect(result.error().code == "EAWR-TEST-RETENTION", "retention failure prevents publishing timing change");
                    failed = true;
                    break;
                }
            }
        }
        expect(failed && retained == 1 && match.sessions[0].input_delay() == 4, "failed retention keeps active delay intact");
    }
    for (const bool measured : {false, true}) {
        AdaptiveOpaque match(1000, false);
        if (!measured) match.transports[0].stats.reset();
        for (std::uint64_t round = 0; round < 650; ++round) {
            match.network.round = round;
            for (auto& session : match.sessions) require(session.pump(round));
        }
        expect(match.sessions[0].input_delay() == (measured ? 8U : 4U)
            && match.sessions[1].input_delay() == match.sessions[0].input_delay(), "transport stats drive host policy; unknown RTT defers it");
        expect(match.transports[0].reads > 0 && match.transports[1].reads == 0, "only host samples real transport stats");
    }
    for (const std::string fault : {"unauthorized", "truncated", "delay", "horizon", "conflict"}) {
        AdaptiveOpaque match;
        auto frame = timing_frame(net::timing_proposal_kind, fault == "horizon" ? 5U : 40U, fault == "delay" ? 2U : 8U);
        if (fault == "truncated") frame.pop_back();
        const auto target = fault == "unauthorized" ? 0U : 1U;
        match.network.deliver(1 - target, target, frame);
        if (fault == "conflict") match.network.deliver(0, 1, timing_frame(net::timing_ready_kind, 40, 6));
        match.network.round = 1;
        const auto result = match.sessions[target].pump(1);
        expect(!result && result.error().code == (fault == "truncated" || fault == "delay"
            ? net::timing_errors::malformed : net::timing_errors::agreement), "adaptive wire refusal: " + fault);
    }
    for (const bool expire : {false, true}) {
        AdaptiveOpaque match(expire ? 100 : 1000);
        match.network.fault = [](auto from, auto to, auto frame) {
            FakeDelivery delivery;
            delivery.drop = from == 1 && to == 0 && frame[0] == net::timing_ready_kind;
            return delivery;
        };
        bool failed{};
        for (std::uint64_t round = 0; round < 350; ++round) {
            match.network.round = round;
            for (unsigned peer = 0; peer < 2; ++peer) {
                const auto result = match.sessions[peer].pump(round);
                if (!result) {
                    expect(expire && result.error().code == net::timing_errors::timeout, "missing timing vote expires explicitly");
                    failed = true;
                    break;
                }
            }
            if (failed) break;
            if (!expire && match.adapters[0].tick == 165 && round > 230) break;
        }
        expect(match.adapters[0].tick == 165 && match.sessions[0].input_delay() == 4,
            "missing readiness holds the exact event tick without applying delay");
        if (expire) expect(failed, "silent timing voter fails closed");
        else {
            match.network.fault = {};
            match.network.deliver(1, 0, timing_frame(net::timing_ready_kind, 165, 8));
            for (std::uint64_t round = match.network.round + 1; round < 900; ++round) {
                match.network.round = round;
                for (auto& session : match.sessions) require(session.pump(round));
                if (match.adapters[0].tick == 500 && match.adapters[1].tick == 500) break;
            }
            expect(match.adapters[0].tick == 500 && match.adapters[1].tick == 500
                && match.sessions[0].input_delay() == 8 && match.sessions[1].input_delay() == 8,
                "delayed readiness resumes both peers losslessly");
        }
    }
}

void mode_neutral_eight_peer_observer() {
    FakeNetwork network(8);
    std::vector<net::PeerSeat> roster;
    for (std::uint32_t peer = 0; peer < 8; ++peer) roster.push_back({sim::SeatId{8 - peer}, peer != 7});
    std::vector<OpaqueAdapter> providers(8);
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<net::LockstepSession> sessions;
    std::array<unsigned, 8> seals{}, receives{};
    net::SessionOptions options;
    options.input_delay = 6;
    options.final_tick_count = 20;
    for (net::PeerIndex peer = 0; peer < 8; ++peer) {
        transports.push_back(std::make_unique<FakeTransport>(network, peer));
        sessions.push_back(require(net::LockstepSession::create(providers[peer], *transports.back(), roster, options)));
        // Observe opposite ends of the roster; the uninstrumented peers provide
        // an independent state/hash comparison and empty batches emit no stages.
        if (peer == 0 || peer == 7) sessions.back().set_stage_observer(
            [&, peer](std::string_view stage, const net::SeatInput& input, std::uint64_t completed) {
                expect(input.key.tick == 6 && input.key.sequence == 0 && completed <= 6,
                    "stage observation retains stamped command key and completed frame");
                if (stage == "seal") ++seals[peer];
                else if (stage == "receive") ++receives[peer];
                else expect(false, "only sealed or received commands emit core stages");
            });
        const auto submitted = sessions.back().submit([](sim::SeatCommandKey key) {
            return eawr::core::Result<std::vector<std::uint8_t>>::success({static_cast<std::uint8_t>(key.seat.value)});
        });
        expect(static_cast<bool>(submitted) == (peer != 7), "observer remains in lockstep but cannot submit");
    }
    for (std::uint64_t round = 0; round < 80; ++round) {
        network.round = round;
        for (auto& session : sessions) require(session.pump(round));
    }
    for (const auto& provider : providers) {
        expect(provider.tick == 20 && provider.history == std::vector<std::uint8_t>({2, 3, 4, 5, 6, 7, 8}),
            "eight mode-neutral providers agree with reversed seat roster and observer");
        expect(provider.state_hash() == providers[0].state_hash(), "provider-supplied final hashes agree");
    }
    expect(seals[0] == 1 && receives[0] == 6 && seals[7] == 0 && receives[7] == 7,
        "stage hooks observe each command once, including on a non-submitting peer");
}

// An adapter that hashes like the live viewer: step() returns only the cheap world part and state_hash()
// adds the script part, which is what a Lua-only divergence changes.
class SplitHashAdapter final : public net::SessionAdapter {
public:
    std::uint64_t tick{};
    std::uint8_t script{};
    std::uint64_t diverge_after{~0ULL};
    mutable std::size_t full_hashes{};
    std::uint64_t completed_tick() const override { return tick; }
    std::string world_hash() const { return eawr::core::sha256_hex(std::vector<std::uint8_t>{static_cast<std::uint8_t>(tick)}); }
    std::string state_hash() const override {
        ++full_hashes;
        return eawr::core::sha256_hex(std::vector<std::uint8_t>{static_cast<std::uint8_t>(tick), script});
    }
    std::vector<std::uint8_t> canonical_state() const override { return {static_cast<std::uint8_t>(tick), script}; }
    eawr::core::Result<void> validate(const net::SeatInput&) const override { return eawr::core::Result<void>::success(); }
    eawr::core::Result<void> install(std::span<const net::SeatInput>) override { return eawr::core::Result<void>::success(); }
    eawr::core::Result<std::vector<std::uint8_t>> canonical_input() const override {
        return eawr::core::Result<std::vector<std::uint8_t>>::success({});
    }
    eawr::core::Result<net::CompletedTick> step() override {
        ++tick;
        if (tick > diverge_after) script = 1;
        return eawr::core::Result<net::CompletedTick>::success({tick, world_hash(), true});
    }
    bool is_departure(const net::SeatInput&) const override { return false; }
};

void script_hash_only_on_exchange_ticks() {
    for (const std::uint64_t diverge_after : {~0ULL, 7ULL, 11ULL}) {
        FakeNetwork network(2);
        std::array<SplitHashAdapter, 2> adapters;
        adapters[1].diverge_after = diverge_after;
        std::vector<std::unique_ptr<FakeTransport>> transports;
        std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
        std::vector<net::LockstepSession> sessions;
        const std::vector<net::PeerSeat> roster{{{1}, true}, {{2}, true}};
        for (unsigned peer = 0; peer < 2; ++peer) {
            transports.push_back(std::make_unique<FakeTransport>(network, peer));
            net::DesyncOptions monitor_options; monitor_options.exchange_every = 5;
            monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), monitor_options));
            net::SessionOptions options; options.final_tick_count = 12;
            sessions.push_back(require(net::LockstepSession::create(adapters[peer], *transports.back(), roster, options, monitors.back().get())));
        }
        for (std::uint64_t round = 0; round < 200; ++round) {
            network.round = round;
            for (unsigned peer = 0; peer < 2; ++peer) require(sessions[peer].pump(round));
        }
        // Tick 0 and the exchange ticks 5, 10 and the end tick 12 hash the scripts; nothing else does.
        // A divergence caught at tick 10 freezes both peers before tick 12.
        const std::size_t hashed = diverge_after == 7 ? 3 : 4;
        expect(adapters[0].full_hashes == hashed && adapters[1].full_hashes == hashed,
            "script hash is computed only at tick zero, exchange ticks and the end tick");
        if (diverge_after == ~0ULL) {
            expect(!monitors[0]->report() && !monitors[1]->report(), "equal script state raises no desync report");
        } else {
            expect(monitors[0]->report() && monitors[1]->report(), "script-only divergence is detected at the next exchange");
            const auto expected = diverge_after < 10 ? 10U : 12U;
            expect(monitors[0]->report()->first_divergent_tick == expected, "script-only divergence is reported at the exchange row");
        }
    }
}
}
using namespace lockstep_session_test;

int main() {
    try {
        script_hash_only_on_exchange_ticks();
        object_turn_only_command_log();
        chained_departures();
        target_timing_controls();
        final_tick_departure_ack();
        deferred_departure_refusals();
        std::string chained_reference;
        std::string settling_reference;
        std::string triple_reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto hash = chained_tactical_departures(workers);
            if (workers == 1) chained_reference = hash;
            expect(hash == chained_reference, "chained tactical departures agree on 1/2/4/8 workers");
            std::cout << "chained departure workers=" << workers << " hash=" << hash << '\n';
            const auto settling_hash = chained_tactical_departures(workers, true);
            if (workers == 1) settling_reference = settling_hash;
            expect(settling_hash == settling_reference, "settle-window departures agree on 1/2/4/8 workers");
            std::cout << "settle-window departure workers=" << workers << " hash=" << settling_hash << '\n';
            const auto triple_hash = triple_tactical_departures(workers);
            if (workers == 1) triple_reference = triple_hash;
            expect(triple_hash == triple_reference, "three-loss settle windows agree on 1/2/4/8 workers");
            std::cout << "three-loss departure workers=" << workers << " hash=" << triple_hash << '\n';
        }
        for (const bool double_loss : {false, true}) {
            std::string reference;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                const auto hash = retained_timing_departure(workers, true, !double_loss, double_loss);
                if (workers == 1) reference = hash;
                expect(hash == reference, "timing retention races agree on 1/2/4/8 workers");
                std::cout << "timing race double=" << double_loss << " workers=" << workers << " hash=" << hash << '\n';
            }
        }
        for (const bool host_leaves : {false, true}) {
            std::string reference;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                const auto hash = retained_timing_departure(workers, host_leaves);
                if (workers == 1) reference = hash;
                expect(hash == reference, "retained timing departure agrees on 1/2/4/8 workers");
                std::cout << "retained timing departure host=" << host_leaves << " workers=" << workers << " hash=" << hash << '\n';
            }
        }
        std::string expiry_reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto hash = retained_timing_departure(workers, true, true, false, true);
            if (workers == 1) expiry_reference = hash;
            expect(hash == expiry_reference, "all-survivor timing expiry refusal agrees on 1/2/4/8 workers");
            std::cout << "timing expiry workers=" << workers << " hash=" << hash << '\n';
        }
        for (const bool double_loss : {false, true}) {
            std::string reference;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                const auto hash = maximum_roster_departures(workers, double_loss);
                if (workers == 1) reference = hash;
                expect(hash == reference, "nine-peer maximum-delay departures agree on 1/2/4/8 workers");
                std::cout << "maximum-roster double=" << double_loss << " workers=" << workers << " hash=" << hash << '\n';
            }
        }
        adaptive_policy_and_refusals();
        adaptive_wire_and_gate();
        adaptive_settled_departure();
        adaptive_settled_departure(true);
        adaptive_settled_departure(false, true);
        adaptive_transition_proposal();
        for (const auto peers : {2U, 3U}) {
            std::string reference;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                const auto hash = adaptive_agreement(workers, peers, true);
                if (workers == 1) reference = hash;
                expect(hash == reference, "adaptive match deterministic on 1/2/4/8 workers");
            }
            expect(adaptive_agreement(1, peers, false) == reference, "timing delivery skew preserves gameplay hashes");
        }
        for (const auto delay : {1U, 3U, 5U}) {
            std::string reference;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                const auto hash = lan_click_latency(delay, workers, 0);
                if (workers == 1) reference = hash;
                expect(hash == reference, "LAN click contract equals across 1/2/4/8 workers");
                expect(lan_click_latency(delay, workers, 1) == reference,
                    "LAN delayed drain preserves click latency and the same state across workers");
                expect(lan_click_latency(delay, workers, 2) == reference,
                    "LAN delayed input construction preserves click latency and the same state across workers");
            }
        }
        for (const auto peers : {2U, 4U}) {
            for (const auto delay : {1U, 3U, 6U}) {
                std::string reference;
                for (const auto workers : {1U, 2U, 4U, 8U}) {
                    const auto hash = agreement(peers, delay, workers);
                    if (workers == 1) reference = hash;
                    expect(hash == reference, "session core agrees across 1/2/4/8 simulation workers");
                }
            }
        }
        for (const std::string fault : {"duplicate", "late_duplicate", "late"}) agreement(2, 3, 4, fault);
        const auto wan_reference = agreement(2, 3, 1);
        for (const auto& profile : eawr::testing::wan_profiles)
            for (const auto workers : {1U, 2U, 4U, 8U})
                expect(agreement(2, 3, workers, {}, profile) == wan_reference,
                    std::string(profile.name) + " retains baseline hash on every worker count");
        allied_last_human_is_named_winner();
        ai_slot_does_not_hold_one_human_match();
        partially_delivered_quit();
        departure_wire_refusals();
        monitored_maximum_lead();
        monitor_send_close_race();
        monitored_silent_peer();
        monitored_departures();
        agreed_departures();
        missing_and_faults();
        strict_codec_and_admission();
        monitor_quit_and_options();
        mode_neutral_eight_peer_observer();
    } catch (const std::exception& error) { expect(false, error.what()); }
    if (failures == 0) std::cout << "LAN session contracts passed\n";
    return failures == 0 ? 0 : 1;
}
