#include "pair_network.hpp"
#include "setup_test_support.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <array>
#include <iostream>
#include <memory>
#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// LockstepSession over an N-peer room mesh with cuttable pairs (docs/net-online-npeer.md, section 3 and the test plan rows
// "Lockstep integration" and "Pair-loss agreement"): the pair-loss decision, its hand-off to the departure protocol, the
// per-pair RTT minimum at the host and the timing freeze. Repair cases compare partitioned adapter tick hashes at 1/2/4/8
// workers here; the same equality over the real tactical simulation is mesh_session_workers_tests.cpp.
namespace {
namespace net = eawr::net;
namespace sim = eawr::sim;
using eawr::testing::PairNetwork;
using eawr::testing::PairTransport;

int failures{};
unsigned test_workers{1};
std::vector<std::string> worker_evidence;
void expect(bool value, const std::string& message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}

class CounterAdapter final : public net::SessionAdapter {
public:
    std::uint64_t tick{};
    bool budget_mode{};
    const sim::PartitionExecutor* executor{};
    std::array<std::uint64_t, 8> partition_digest{};
    std::vector<std::uint8_t> history;
    std::vector<std::string> tick_hashes;
    std::uint64_t completed_tick() const override { return tick; }
    std::string state_hash() const override { return eawr::core::sha256_hex(canonical_state()); }
    std::vector<std::uint8_t> canonical_state() const override {
        auto bytes = history;
        for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(tick >> (8 * i)));
        for (const auto shard : partition_digest)
            for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(shard >> (8 * i)));
        return bytes;
    }
    eawr::core::Result<void> validate(const net::SeatInput& input) const override {
        if ((input.bytes.size() == 1 || (budget_mode && !input.bytes.empty() && input.bytes.size() < net::max_batch_bytes))
            && (input.bytes.front() == input.key.seat.value || input.bytes.front() == 0))
            return eawr::core::Result<void>::success();
        eawr::core::Diagnostic error;
        error.code = net::session_errors::admission; error.message = "refused";
        return eawr::core::Result<void>::failure(std::move(error));
    }
    eawr::core::Result<void> install(std::span<const net::SeatInput> commands) override {
        if (budget_mode) return eawr::core::Result<void>::success();
        history.push_back(0xAA);
        for (const auto& input : commands) {
            history.push_back(static_cast<std::uint8_t>(input.key.seat.value));
            history.insert(history.end(), input.bytes.begin(), input.bytes.end());
        }
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<std::vector<std::uint8_t>> canonical_input() const override {
        return eawr::core::Result<std::vector<std::uint8_t>>::success({});
    }
    eawr::core::Result<net::CompletedTick> step() override {
        const auto input = history; // Immutable copied input, fixed logical partitions, disjoint staging.
        std::array<std::uint64_t, 8> staged{};
        auto ran = executor->execute(staged.size(), [&](std::size_t partition) {
            std::uint64_t digest = 1469598103934665603ULL;
            for (std::size_t index = partition; index < input.size(); index += staged.size())
                digest = (digest ^ input[index]) * 1099511628211ULL;
            staged[partition] = digest;
        });
        if (!ran) return eawr::core::Result<net::CompletedTick>::failure(ran.error());
        partition_digest = staged; // Ordered commit; the network never runs in a partition.
        ++tick;
        tick_hashes.push_back(state_hash());
        return eawr::core::Result<net::CompletedTick>::success({tick, tick_hashes.back()});
    }
    bool is_departure(const net::SeatInput& input) const override { return input.bytes.size() == 1 && input.bytes.front() == 0; }
};

void three_seat_online_start_barrier() {
    // Match the viewer: online mesh policy is already configured during Start,
    // while departure encoding is installed only after setup transfers its core.
    constexpr net::PeerIndex count = 3;
    PairNetwork network(count);
    eawr::platform::ThreadWorkerAdapter executor(test_workers);
    std::vector<std::unique_ptr<PairTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<net::SetupSession> staged;
    auto selected = setup_test::offer();
    selected.setup = require(net::encode_skirmish_setup(setup_test::human_setup(count)));
    selected.seats = {{0, sim::SeatId{1}, 1}, {1, sim::SeatId{2}, 2}, {2, sim::SeatId{3}, 3}};
    selected.input_delay = 1; selected.final_tick_count = 4;
    net::SetupOptions options;
    options.auto_start = false; options.auto_accept = false;
    options.session.final_tick_count = selected.final_tick_count;
    options.session.mesh.emplace();
    for (net::PeerIndex peer = 0; peer < count; ++peer) {
        transports.push_back(std::make_unique<PairTransport>(network, peer));
        net::DesyncOptions monitor_options; monitor_options.input_delay = selected.input_delay;
        monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), monitor_options));
        auto* monitor = monitors.back().get();
        net::SetupProviders provider;
        provider.monitor = monitor;
        provider.identify = [identity = selected.identity](const auto&) {
            return eawr::core::Result<sim::MatchAgreement>::success(identity);
        };
        provider.build = [&executor](const auto&) -> eawr::core::Result<std::unique_ptr<net::SessionAdapter>> {
            auto adapter = std::make_unique<CounterAdapter>(); adapter->executor = &executor;
            return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(adapter));
        };
        provider.tick_zero_agreed = [monitor] { return monitor->setup_agreed(); };
        provider.tick_zero_mismatch = [] { return std::optional<net::TickZeroMismatch>{}; };
        staged.push_back(peer == 0
            ? require(net::SetupSession::host(*transports.back(), selected, std::move(provider), options))
            : require(net::SetupSession::join(*transports.back(), std::move(provider), options)));
    }
    require(staged.front().start());
    bool ready{};
    for (unsigned round = 0; round < 100 && !ready; ++round) {
        ++network.round;
        for (auto& session : staged) {
            require(session.pump(network.round));
            if (session.phase() == net::SetupPhase::reviewing_offer) require(session.accept());
        }
        ready = std::all_of(staged.begin(), staged.end(), [](const auto& session) { return session.phase() == net::SetupPhase::ready; });
    }
    expect(ready, "three online seats complete Start with mesh policy before departure encoding is installed");
    std::vector<net::SetupResult> running;
    for (auto& session : staged) {
        running.push_back(require(session.take_result()));
        expect(running.back().adapter().completed_tick() == 0, "the Start barrier never advanced simulation");
        running.back().session().enable_departures([](auto) { return eawr::core::Result<std::vector<std::uint8_t>>::success({0}); });
    }
    for (unsigned round = 0; round < 100; ++round) {
        ++network.round;
        for (auto& result : running) require(result.session().pump(network.round));
    }
    for (auto& result : running) {
        expect(result.adapter().completed_tick() == selected.final_tick_count, "all three seats advance after Start commits");
        expect(result.adapter().state_hash() == running.front().adapter().state_hash(), "the three online Start peers finish identically");
        std::string evidence;
        for (const auto& hash : static_cast<const CounterAdapter&>(result.adapter()).tick_hashes) evidence += hash + ",";
        worker_evidence.push_back(std::move(evidence));
    }
}

class SetupGate final : public net::SessionMonitor {
public:
    bool open{};
    bool needs_encoding(std::uint64_t, bool) const override { return false; }
    eawr::core::Result<void> record(std::uint64_t, std::string_view, std::span<const std::uint8_t>,
        std::span<const std::uint8_t>, bool) override { return eawr::core::Result<void>::success(); }
    eawr::core::Result<void> receive(const net::Received&) override { return eawr::core::Result<void>::success(); }
    eawr::core::Result<void> on_link_repair(net::PeerIndex, std::function<bool(net::PeerIndex)> = {}) override {
        return eawr::core::Result<void>::success();
    }
    bool may_step() const override { return open; }
};

void departure_encoding_remains_required_after_setup() {
    PairNetwork network(3);
    PairTransport transport(network, 0);
    CounterAdapter adapter;
    SetupGate gate;
    net::SessionOptions options; options.mesh.emplace();
    const std::vector<net::PeerSeat> seats{{sim::SeatId{1}, true}, {sim::SeatId{2}, true}, {sim::SeatId{3}, true}};
    auto session = require(net::LockstepSession::create(adapter, transport, seats, options, &gate));
    require(session.pump(0));
    expect(adapter.tick == 0, "a gated setup can pump tick zero before departure encoding is installed");
    gate.open = true;
    const auto opened = session.pump(1);
    expect(!opened && opened.error().code == net::mesh_errors::options, "opening the gate without a departure encoder still fails closed");

    CounterAdapter advanced;
    gate.open = false;
    auto past_zero = require(net::LockstepSession::create(advanced, transport, seats, options, &gate));
    advanced.tick = 1;
    const auto pumped = past_zero.pump(0);
    expect(!pumped && pumped.error().code == net::mesh_errors::options, "a gate cannot defer missing departure encoding after tick zero");
}

struct Options {
    std::uint64_t input_delay{4}, final_tick{80};
    std::chrono::milliseconds grace{2000};
    bool adaptive{}, monitored{};
    std::function<std::optional<std::uint32_t>(net::PeerIndex self, net::PeerIndex remote)> rtt;
    std::uint64_t pump_every{1}; // rounds (fake milliseconds) between pumps: 33 is the 30 Hz game
    int lag_seat{-1};            // this seat pumps only every lag_every-th round: a slow machine
    std::uint64_t lag_every{1};
    std::uint64_t completeness_timeout{100'000}; // ms of a missing batch before an ordinary timeout departure starts
    unsigned slow_timeout_seats{0};              // these seats never time out on their own: they learn of timeout departures from the others
    int stall_seat{-1};                          // this seat stops pumping at stall_from (a frozen but still connected machine)
    std::uint64_t stall_from{0};
};

// GCC 14 reports the std::function destructor inside the engaged-optional emplace of SessionOptions::adaptive_timing as
// maybe-uninitialized (a false positive: the optional starts empty); the identical assignment in lockstep_session_tests.cpp
// is not inlined the same way.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
struct Rig {
    Rig(unsigned count, Options settings) : network(count), setup(settings), executor(test_workers), adapters(count), failed(count), departures(count) {
        monitors.resize(count);
        for (auto& adapter : adapters) adapter.executor = &executor;
        std::vector<net::PeerSeat> seats;
        for (unsigned seat = 0; seat < count; ++seat) seats.push_back({sim::SeatId{seat + 1}, true});
        for (net::PeerIndex seat = 0; seat < count; ++seat) {
            transports.push_back(std::make_unique<PairTransport>(network, seat));
            net::SessionOptions options;
            options.input_delay = settings.input_delay; options.final_tick_count = settings.final_tick;
            options.completeness_timeout_ms = (settings.slow_timeout_seats >> seat) & 1U ? 100'000'000 : settings.completeness_timeout;
            net::MeshIncidentOptions mesh;
            mesh.repair_grace = settings.grace;
            if (settings.rtt) mesh.rtt_ms = [&, seat](net::PeerIndex remote) { return setup.rtt(seat, remote); };
            options.mesh = mesh;
            if (settings.adaptive) { options.non_lan = true; options.adaptive_timing.emplace(); } // host 0, broadband, link-stats RTT
            if (settings.monitored) {
                net::DesyncOptions monitoring; monitoring.input_delay = settings.input_delay; monitoring.exchange_every = 10;
                monitors[seat] = std::make_unique<net::DesyncMonitor>(*transports.back(), monitoring);
            }
            sessions.push_back(require(net::LockstepSession::create(adapters[seat], *transports.back(), seats, options, monitors[seat].get())));
            sessions.back().enable_departures([](auto) { return eawr::core::Result<std::vector<std::uint8_t>>::success({0}); });
        }
    }
    ~Rig() {
        std::string receipt;
        for (std::size_t peer = 0; peer < adapters.size(); ++peer) {
            receipt += std::to_string(adapters[peer].tick) + ":" + adapters[peer].state_hash() + ":" + failed[peer] + ":";
            for (const auto& hash : adapters[peer].tick_hashes) receipt += hash + ",";
            receipt += ":";
            for (const auto& event : departures[peer]) receipt += std::to_string(event.key.tick) + "/"
                + std::to_string(event.key.seat.value) + "/" + std::to_string(static_cast<unsigned>(event.reason)) + ",";
            receipt += ";";
        }
        worker_evidence.push_back(std::move(receipt));
    }
    void round(std::uint64_t submit_until = ~0ULL) {
        ++now;
        network.round = now;
        if (now % setup.pump_every != 0) return;
        for (net::PeerIndex seat = 0; seat < sessions.size(); ++seat) {
            if (!failed[seat].empty()) continue;
            if (static_cast<int>(seat) == setup.lag_seat && now % setup.lag_every != 0) continue;
            if (static_cast<int>(seat) == setup.stall_seat && now >= setup.stall_from) continue;
            if (adapters[seat].tick < submit_until && adapters[seat].tick + setup.input_delay < setup.final_tick && !sessions[seat].failure())
                static_cast<void>(sessions[seat].submit([](sim::SeatCommandKey key) {
                    return eawr::core::Result<std::vector<std::uint8_t>>::success({static_cast<std::uint8_t>(key.seat.value)});
                }));
            auto pumped = sessions[seat].pump(now);
            if (!pumped) { failed[seat] = pumped.error().code; continue; }
            for (const auto& event : pumped.value().departure_events) departures[seat].push_back(event);
        }
    }
    template<class F> bool until(F predicate, std::uint64_t budget) {
        const auto end = now + budget;
        while (now < end) { if (predicate()) return true; round(); }
        return predicate();
    }
    PairNetwork network;
    Options setup;
    std::uint64_t now{};
    eawr::platform::ThreadWorkerAdapter executor;
    std::vector<CounterAdapter> adapters;
    std::vector<std::unique_ptr<PairTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<net::LockstepSession> sessions;
    std::vector<std::string> failed;
    std::vector<std::vector<net::DepartureEvent>> departures;
};
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

void partitioned_pair_loss() {
    // Seats a<b<c<d: pairs ab, ac, bc are lost mid-battle. d is the only all-open seat and decides; a and d continue,
    // run the existing departure protocol for b then c with the pair-loss reason and finish the match identically.
    Rig rig(4, {});
    while (rig.now < 30) rig.round();
    const auto at_cut = rig.adapters[3].tick;
    expect(at_cut > 5 && at_cut < 60, "the cut lands mid-battle");
    rig.network.cut(0, 1); rig.network.cut(0, 2); rig.network.cut(1, 2);
    expect(rig.until([&] { return rig.adapters[0].tick == 80 && rig.adapters[3].tick == 80; }, 90'000),
        "the survivors finish the match after the agreed departures");
    expect(rig.failed[0].empty() && rig.failed[3].empty(), "survivors never failed: " + rig.failed[0] + rig.failed[3]);
    for (const auto seat : {1U, 2U})
        expect(rig.failed[seat] == net::mesh_errors::left, "a removed seat leaves with the pair-loss reason: " + rig.failed[seat]);
    expect(rig.adapters[0].state_hash() == rig.adapters[3].state_hash(), "survivor tick hashes are equal");
    for (const auto seat : {0U, 3U}) {
        expect(rig.departures[seat].size() == 2, "each survivor reports both departures");
        if (rig.departures[seat].size() == 2) {
            expect(rig.departures[seat][0].reason == net::DepartureReason::pair_loss && rig.departures[seat][1].reason == net::DepartureReason::pair_loss,
                "the departures carry the pair-loss reason");
            expect((rig.departures[seat][0].key.seat == sim::SeatId{2} && rig.departures[seat][1].key.seat == sim::SeatId{3})
                || (rig.departures[seat][0].key.seat == sim::SeatId{3} && rig.departures[seat][1].key.seat == sim::SeatId{2}),
                "b and c are the removed seats");
        }
        const auto& results = rig.sessions[seat].mesh_results();
        expect(results.size() == 1 && results.front().removed == 0b0110 && results.front().survivors == 0b1001,
            "the session keeps the incident result for the notice");
        expect(!rig.sessions[seat].mesh_running(), "the incident settled once both departures were agreed");
    }
    expect(rig.departures[0] == rig.departures[3], "both survivors agree every cutoff tick");
}

void no_decider_ends_the_match() {
    Rig rig(4, {});
    while (rig.now < 30) rig.round();
    rig.network.cut(0, 1); rig.network.cut(2, 3);
    rig.until([&] { return false; }, 1'000);
    std::array<std::uint64_t, 4> stalled{};
    for (unsigned seat = 0; seat < 4; ++seat) stalled[seat] = rig.adapters[seat].tick;
    rig.until([&] { return false; }, 40'000);
    for (unsigned seat = 0; seat < 4; ++seat) {
        expect(rig.failed[seat] == net::mesh_errors::no_majority, "every seat leaves with 'nobody could decide': " + rig.failed[seat]);
        expect(rig.adapters[seat].tick == stalled[seat] && rig.adapters[seat].tick < 80, "nobody steps past the stall tick");
        expect(rig.departures[seat].empty(), "no departure was agreed");
    }
}

const auto quit_command = [](sim::SeatCommandKey) { return eawr::core::Result<std::vector<std::uint8_t>>::success({0}); };

void ordinary_quit_never_starts_an_incident(unsigned count) {
    // The quitter executes its quit tick and its transport closes while one survivor still lags behind that tick. The lagging
    // seat has the quitter's sealed quit already, so the closed link is an ordinary departure, not a pair loss: no incident
    // starts and nobody leaves (a shared active set, not each seat's own bookkeeping).
    Options settings;
    settings.lag_seat = static_cast<int>(count) - 2; settings.lag_every = 9;
    Rig rig(count, settings);
    const net::PeerIndex quitter = count - 1;
    while (rig.now < 30) rig.round();
    static_cast<void>(rig.sessions[quitter].submit(quit_command));
    bool closed{};
    const auto survivors_done = [&] {
        for (net::PeerIndex seat = 0; seat < quitter; ++seat) if (rig.adapters[seat].tick != 80) return false;
        return true;
    };
    rig.until([&] {
        if (!closed && !rig.departures[quitter].empty()) { closed = true; rig.network.isolate(quitter); }
        return survivors_done();
    }, 600'000);
    expect(closed, "the quitter executed its quit tick: " + std::to_string(count) + " seats");
    for (net::PeerIndex seat = 0; seat < quitter; ++seat) {
        expect(rig.failed[seat].empty(), "survivor " + std::to_string(seat) + " of " + std::to_string(count) + " finishes: " + rig.failed[seat]);
        expect(rig.sessions[seat].mesh_results().empty() && !rig.sessions[seat].mesh_running(), "an ordinary quit starts no pair-loss incident");
        expect(rig.departures[seat].size() == 1 && rig.departures[seat].front().reason == net::DepartureReason::quit
            && rig.departures[seat].front().key.seat == sim::SeatId{quitter + 1}, "the departure is the quit, not a pair loss");
        expect(rig.adapters[seat].state_hash() == rig.adapters[0].state_hash(), "survivor tick hashes are equal");
    }
}

void ordinary_timeout_ends_without_an_invented_departure() {
    // Application silence is not a confirmed pair-loss decision, even on a healthy mesh.
    Options settings;
    settings.completeness_timeout = 3'000; settings.stall_seat = 3; settings.stall_from = 30;
    Rig rig(4, settings);
    expect(rig.until([&] { return !rig.failed[0].empty() && !rig.failed[1].empty() && !rig.failed[2].empty(); }, 200'000),
        "all still-running seats reach their bounded connection-loss deadline");
    for (const net::PeerIndex seat : {0U, 1U, 2U}) {
        expect(rig.failed[seat] == net::session_errors::stalled, "application timeout is connection loss on every running seat");
        expect(rig.departures[seat].empty() && rig.adapters[seat].tick < 80,
            "unconfirmed application silence cannot manufacture a removed seat or let the match finish");
    }
}

// Cut pairs in the middle of a match; `survivors` finish it identically and every `leavers` seat ends with exactly its code.
void cut_scenario(const std::string& label, unsigned count, const std::vector<std::pair<unsigned, unsigned>>& cuts,
        const std::vector<unsigned>& survivors, const std::vector<std::pair<unsigned, std::string_view>>& leavers) {
    Rig rig(count, {});
    while (rig.now < 30) rig.round();
    for (const auto& [a, b] : cuts) rig.network.cut(a, b);
    const auto done = [&] {
        for (const auto seat : survivors) if (rig.adapters[seat].tick != 80) return false;
        for (const auto& [seat, code] : leavers) if (rig.failed[seat].empty()) return false;
        return true;
    };
    expect(rig.until(done, 200'000), label + ": the survivors finish and the removed seats end");
    for (const auto seat : survivors) {
        expect(rig.failed[seat].empty(), label + ": survivor " + std::to_string(seat) + " never fails: " + rig.failed[seat]);
        expect(rig.adapters[seat].state_hash() == rig.adapters[survivors.front()].state_hash(), label + ": survivor hashes are equal");
        std::set<std::uint32_t> gone;
        for (const auto& event : rig.departures[seat]) {
            expect(event.reason == net::DepartureReason::pair_loss, label + ": the departures carry the pair-loss reason");
            gone.insert(event.key.seat.value);
        }
        std::set<std::uint32_t> expected;
        for (const auto& [seat_index, code] : leavers) { static_cast<void>(code); expected.insert(seat_index + 1); }
        expect(gone == expected, label + ": exactly the removed seats departed at survivor " + std::to_string(seat));
        expect(rig.sessions[seat].mesh_results().size() == 1, label + ": one incident");
    }
    for (const auto& [seat, code] : leavers) expect(rig.failed[seat] == code, label + ": seat " + std::to_string(seat) + " ends with " + std::string(code) + ", got " + rig.failed[seat]);
}

void decider_loss_ends_the_match() {
    // The only all-open seat (d) decides, collects a pledge and drops before it can confirm. Nobody may continue on a private
    // survivor set and nobody may hang: every seat ends with a typed reason inside the agreed windows.
    Rig rig(4, {});
    while (rig.now < 30) rig.round();
    bool dropped{};
    unsigned pledges{};
    rig.network.on_send = [&](net::PeerIndex, net::PeerIndex to, std::span<const std::uint8_t> frame) {
        // The decider dies as the last of the three pledges is sent: every seat accepted and pledged, nobody gets a confirmation.
        if (!dropped && frame.front() == net::mesh_pledge_kind && to == 3 && ++pledges == 3) { dropped = true; rig.network.isolate(3); }
    };
    rig.network.cut(0, 1); rig.network.cut(0, 2); rig.network.cut(1, 2);
    rig.until([&] { return std::all_of(rig.failed.begin(), rig.failed.end(), [](const auto& code) { return !code.empty(); }); }, 120'000);
    expect(dropped, "the decider was reached by a pledge and then dropped");
    for (unsigned seat = 0; seat < 4; ++seat) {
        // Seats a, b and c accepted d's proposal (victims b and c included) and none received a confirmation: the exact reason
        // is "the decision did not complete", never "removed" (that is for a seat a confirmation named).
        if (seat < 3) expect(rig.failed[seat] == net::mesh_errors::no_decision, "seat " + std::to_string(seat) + " ends with 1781, not a hang: " + rig.failed[seat]);
        else expect(!rig.failed[seat].empty(), "the isolated decider ends too: " + rig.failed[seat]);
        expect(rig.adapters[seat].tick < 80 && rig.departures[seat].empty(), "nobody continues or agrees a departure without a confirmation");
    }
}

void pair_loss_proposal_needs_a_decision() {
    // A survivor-style proposal for a seat nobody removed: held back, then refused when no decision ever arrives.
    Rig rig(3, {});
    while (rig.now < 30) rig.round();
    std::vector<std::uint8_t> frame{net::departure_proposal_kind, 2, 0, 0, 0, static_cast<std::uint8_t>(net::DepartureReason::pair_loss)};
    for (unsigned i = 0; i < 8; ++i) frame.push_back(0);
    rig.network.deliver(1, 0, frame);
    rig.until([&] { return false; }, 5'000);
    expect(rig.failed[0].empty(), "an early pair-loss proposal waits for the decision instead of failing at once");
    rig.until([&] { return !rig.failed[0].empty(); }, 60'000);
    expect(rig.failed[0] == net::departure_errors::agreement, "without an accepted decision it is refused: " + rig.failed[0]);
    // A proposal against ourselves or with an unknown reason is malformed at once.
    Rig other(3, {});
    while (other.now < 30) other.round();
    std::vector<std::uint8_t> bad{net::departure_proposal_kind, 0, 0, 0, 0, 3};
    for (unsigned i = 0; i < 8; ++i) bad.push_back(0);
    other.network.deliver(1, 0, bad);
    other.until([&] { return !other.failed[0].empty(); }, 1'000);
    expect(other.failed[0] == net::departure_errors::malformed, "a pair-loss proposal naming the receiver is malformed: " + other.failed[0]);
}

std::uint32_t model_worst(const std::function<std::uint32_t(unsigned, unsigned)>& claim, unsigned count) {
    std::uint32_t worst{};
    for (unsigned a = 0; a < count; ++a) for (unsigned b = a + 1; b < count; ++b)
        worst = std::max(worst, a == 0 ? claim(0, b) : std::min(claim(a, b), claim(b, a)));
    return worst;
}

void host_timing_takes_the_per_pair_minimum() {
    constexpr unsigned count = 4;
    const std::uint32_t truth[count][count] = {{0, 40, 80, 60}, {40, 0, 100, 300}, {80, 100, 0, 90}, {60, 300, 90, 0}};
    for (const unsigned liar : {0U, 1U, 2U}) { // none, seat 3 claims a huge RTT, seat 3 claims none
        const auto claim = [&](unsigned from, unsigned to) -> std::uint32_t {
            if (from == 3 && liar == 1) return 5000;
            if (from == 3 && liar == 2) return 1;
            return truth[from][to];
        };
        Options settings;
        settings.adaptive = true; settings.final_tick = 420; settings.pump_every = 33;
        settings.rtt = [&](net::PeerIndex self, net::PeerIndex remote) -> std::optional<std::uint32_t> { return claim(self, remote); };
        Rig rig(count, settings);
        while (rig.now < 33 * 480) rig.round();
        const auto expected = net::adaptive_input_delay(model_worst(claim, count));
        for (unsigned seat = 0; seat < count; ++seat) {
            expect(rig.failed[seat].empty(), "adaptive mesh run stays healthy: " + rig.failed[seat]);
            expect(rig.sessions[seat].input_delay() == expected, "liar mode " + std::to_string(liar) + " seat " + std::to_string(seat)
                + ": delay " + std::to_string(rig.sessions[seat].input_delay()) + " vs " + std::to_string(expected));
        }
        if (liar == 0) expect(expected == 6, "the honest numbers move the delay off its start (6 for a 300 ms worst pair)");
        if (liar == 1) expect(expected == 6, "a seat inflating its claims cannot raise the delay (the other end of each pair says otherwise)");
    }
}

void repaired_pair_replays_its_own_tail() {
    Rig rig(4, {});
    while (rig.now < 30) rig.round();
    rig.network.cut(0, 1);
    while (rig.now < 500) rig.round();
    std::array<std::uint64_t, 4> stalled{};
    for (unsigned seat = 0; seat < 4; ++seat) stalled[seat] = rig.adapters[seat].tick;
    rig.network.heal(0, 1);
    while (rig.now < 1000) rig.round();
    for (unsigned seat = 0; seat < 4; ++seat)
        expect(rig.adapters[seat].tick == stalled[seat], "an open repaired pair cannot bypass the restoration confirmation");
    expect(rig.until([&] { return rig.adapters[0].tick == 80 && rig.adapters[1].tick == 80
        && rig.adapters[2].tick == 80 && rig.adapters[3].tick == 80; }, 15'000), "authenticated replacement tail fills every sealed gap");
    for (unsigned seat = 0; seat < 4; ++seat) {
        expect(rig.failed[seat].empty(), "repair has no stream-gap/fatal-send error: " + rig.failed[seat]);
        expect(rig.departures[seat].empty(), "restored pairs cause no departure");
        expect(rig.adapters[seat].state_hash() == rig.adapters[0].state_hash(), "repaired peers end at equal hashes");
    }
}
void bounded_tail_overflow_uses_departure() {
    Rig rig(3, {});
    for (auto& adapter : rig.adapters) adapter.budget_mode = true;
    std::optional<std::uint64_t> last_large_tick;
    while (rig.now < 25) {
        if (rig.now < 16 && (!last_large_tick || rig.adapters[0].tick + rig.setup.input_delay > *last_large_tick)) {
            const auto key = require(rig.sessions[0].submit([](sim::SeatCommandKey) {
                return eawr::core::Result<std::vector<std::uint8_t>>::success(std::vector<std::uint8_t>(900'000, 1));
            }));
            last_large_tick = key.tick;
        }
        rig.round(0);
    }
    rig.network.cut(0, 1);
    while (rig.now < 500) rig.round();
    rig.network.heal(0, 1);
    expect(rig.until([&] { return rig.adapters[0].tick == 80 && rig.adapters[2].tick == 80; }, 70'000),
        "a replacement whose tail exceeded the cap ends in agreed departure");
    expect(rig.network.failed_repairs[{0, 1}] == 1794, "tail overflow closes the candidate with its typed reason");
    expect(rig.failed[0].empty() && rig.failed[2].empty() && rig.failed[1] == net::mesh_errors::left,
        "the bounded-tail fallback keeps the agreed survivors and removes the later seat");
}

void late_noticing_send_failure_uses_the_mesh(bool monitored = false) {
    Options options; options.monitored = monitored;
    Rig rig(3, options);
    while (rig.now < 30) rig.round();
    rig.network.cut(0, 1, 0, 100'000'000);
    expect(rig.until([&] { return !rig.failed[1].empty(); }, 70'000), "the late-noticing seat reaches a typed decision");
    expect(rig.until([&] {
        for (unsigned peer = 0; peer < 3; ++peer) if (rig.failed[peer].empty() && rig.adapters[peer].tick != 80) return false;
        return true;
    }, 30'000), "every late-noticing seat reaches a terminal outcome");
    std::vector<unsigned> survivors;
    for (unsigned peer = 0; peer < 3; ++peer) if (rig.failed[peer].empty()) survivors.push_back(peer);
    expect(survivors.empty() || survivors.size() >= 2, "a continuing late-noticing clique has at least two seats");
    for (const auto peer : survivors)
        expect(rig.adapters[peer].state_hash() == rig.adapters[survivors.front()].state_hash(), "late-noticing survivors have equal hashes");
    for (const auto& code : rig.failed) expect(code.empty() || code == net::mesh_errors::left
        || code == net::mesh_errors::no_majority || code == net::mesh_errors::no_decision,
        "late-noticing sends end in a typed mesh decision: " + code);
}
void repaired_pair_preserves_an_ordinary_quit() {
    Rig rig(4, {});
    while (rig.now < 30) rig.round();
    const auto quit = require(rig.sessions[3].submit([](sim::SeatCommandKey) {
        return eawr::core::Result<std::vector<std::uint8_t>>::success({0});
    }));
    rig.round(); rig.network.cut(0, 1);
    expect(rig.until([&] { return rig.adapters[3].tick > quit.tick; }, 100), "quitter executes before its close");
    rig.network.isolate(3);
    while (rig.now < 500) rig.round();
    rig.network.heal(0, 1);
    expect(rig.until([&] { return rig.adapters[0].tick == 80 && rig.adapters[1].tick == 80 && rig.adapters[2].tick == 80; }, 60'000),
        "lost ordinary-departure controls are restored over the repaired pair");
    for (const unsigned seat : {0U, 1U, 2U}) {
        expect(rig.failed[seat].empty(), "ordinary-quit repair never fails: " + rig.failed[seat]);
        expect(rig.departures[seat].size() == 1 && rig.departures[seat].front().reason == net::DepartureReason::quit,
            "only the original quitter departs after a successful pair repair");
        expect(rig.adapters[seat].state_hash() == rig.adapters[0].state_hash(), "quit and repair preserve equal hashes");
    }
}
void repaired_pair_preserves_pending_timing() {
    Options options; options.adaptive = true; options.final_tick = 420; options.pump_every = 33;
    options.rtt = [](net::PeerIndex, net::PeerIndex) -> std::optional<std::uint32_t> { return 300; };
    Rig rig(3, options);
    std::optional<std::uint64_t> cut_at;
    rig.network.on_send = [&](net::PeerIndex from, net::PeerIndex to, std::span<const std::uint8_t> frame) {
        if (!cut_at && from == 0 && to == 1 && !frame.empty() && frame.front() == net::timing_proposal_kind) {
            cut_at = rig.now; rig.network.cut(0, 1);
        }
    };
    bool healed = false;
    while (rig.now < 70'000 && (rig.adapters[0].tick != 420 || rig.adapters[1].tick != 420 || rig.adapters[2].tick != 420)) {
        if (cut_at && !healed && rig.now >= *cut_at + 500) { rig.network.heal(0, 1); healed = true; }
        rig.round();
    }
    expect(cut_at && healed, "the repair crosses an in-flight timing proposal");
    for (const unsigned seat : {0U, 1U, 2U}) {
        expect(rig.failed[seat].empty() && rig.adapters[seat].tick == 420, "pending timing survives repair: " + rig.failed[seat]);
        expect(rig.sessions[seat].input_delay() == rig.sessions[0].input_delay(), "repaired peers agree the adaptive delay");
        expect(rig.adapters[seat].state_hash() == rig.adapters[0].state_hash(), "repaired timing retains equal final hashes");
    }
}

void settled_vote_replay_requires_an_exact_vote() {
    for (const bool replacement : {false, true}) {
        Rig rig(3, {});
        // A pre-battle grant already used generation two; that alone is not a repair.
        rig.network.generations[{0, 1}] = rig.network.generations[{1, 0}] = 1;
        while (rig.now < 30) rig.round();
        std::vector<std::uint8_t> proposal;
        rig.network.on_send = [&](net::PeerIndex from, net::PeerIndex to, std::span<const std::uint8_t> frame) {
            if (from == 1 && to == 0 && !frame.empty() && frame.front() == net::departure_proposal_kind)
                proposal.assign(frame.begin(), frame.end());
        };
        rig.network.isolate(2);
        expect(rig.until([&] { return rig.adapters[0].tick == 80 && rig.adapters[1].tick == 80; }, 70'000), "the original departure settles");
        expect(proposal.size() == 14, "the settled voter proposal was captured");
        if (proposal.size() != 14) return;
        if (replacement) {
            rig.network.heal(0, 1); rig.round();
            rig.network.deliver(1, 0, proposal);
            for (unsigned round = 0; round < 5; ++round) rig.round();
            expect(rig.failed[0].empty(), "a replacement accepts the identical settled proposal");
        }
        proposal[6] ^= 1; // A different, still bounded receive frontier.
        rig.network.deliver(1, 0, proposal);
        expect(rig.until([&] { return !rig.failed[0].empty(); }, 20), "a contradictory settled proposal is refused");
        expect(rig.failed[0] == net::departure_errors::agreement, "a grant or repair cannot authorize a changed vote");
    }
}

void unsupported_monitor_fails_before_play() {
    class Unsupported final : public net::SessionMonitor {
    public:
        bool needs_encoding(std::uint64_t, bool) const override { return false; }
        eawr::core::Result<void> record(std::uint64_t, std::string_view, std::span<const std::uint8_t>, std::span<const std::uint8_t>, bool) override {
            return eawr::core::Result<void>::success();
        }
        eawr::core::Result<void> receive(const net::Received&) override { return eawr::core::Result<void>::success(); }
        bool may_step() const override { return true; }
    } monitor;
    CounterAdapter adapter;
    PairNetwork network(3); PairTransport transport(network, 0);
    net::SessionOptions options; options.mesh.emplace();
    const auto result = net::LockstepSession::create(adapter, transport,
        {{sim::SeatId{1}, true}, {sim::SeatId{2}, true}, {sim::SeatId{3}, true}}, options, &monitor);
    expect(!result && result.error().code == "EAWR-NET-1790", "a monitor wrapper missing repair support fails before tick zero");
}

void confirmed_departure_keeps_its_timeout() {
    Options options; options.completeness_timeout = 2000;
    Rig rig(3, options);
    while (rig.now < 30) rig.round();
    rig.network.cut(0, 1);
    bool stopped = false;
    rig.network.on_send = [&](auto, auto, std::span<const std::uint8_t> frame) {
        if (!stopped && !frame.empty() && frame.front() == net::departure_proposal_kind) {
            stopped = true; rig.setup.stall_seat = 2; rig.setup.stall_from = rig.now;
        }
    };
    rig.until([&] { return rig.failed[0] == net::departure_errors::timeout; }, 60'000);
    expect(stopped && rig.failed[0] == net::departure_errors::timeout,
        "a confirmed pair-loss departure still has a bounded settlement timeout");
}

void ordinary_departure_waits_for_the_incident() {
    Options options; options.grace = std::chrono::seconds(35); options.completeness_timeout = 30'000;
    Rig rig(4, options);
    while (rig.now < 30) rig.round();
    // The quitter's sealed batch reaches everyone before an unrelated pair dies.
    const auto quit = require(rig.sessions[3].submit([](sim::SeatCommandKey) {
        return eawr::core::Result<std::vector<std::uint8_t>>::success({0});
    }));
    rig.round(); rig.network.cut(0, 1);
    expect(rig.until([&] { return rig.adapters[3].tick > quit.tick; }, 100), "the quitter executes its already sealed quit");
    rig.network.isolate(3);
    rig.until([&] { return rig.adapters[0].tick == 80 && rig.adapters[2].tick == 80; }, 65'000);
    for (const auto& code : rig.failed) expect(code != net::departure_errors::timeout,
        "an ordinary departure never races the incident grace: " + code);
    expect(rig.adapters[0].tick == 80 && rig.adapters[2].tick == 80,
        "ordinary departure and pair-loss departure both settle at the surviving clique");
    expect(rig.adapters[0].state_hash() == rig.adapters[2].state_hash(), "overlapping departure survivors agree their final state");
}

void host_loss_freezes_timing() {
    Options settings;
    settings.adaptive = true; settings.final_tick = 420; settings.pump_every = 33;
    // A worst pair that would move the delay if anyone proposed; only the host proposes.
    settings.rtt = [](net::PeerIndex, net::PeerIndex) -> std::optional<std::uint32_t> { return 300; };
    Rig rig(4, settings);
    // The host quits early in the match (byte 0 is the synthetic quit of this adapter): an ordinary agreed departure.
    while (rig.now < 33 * 30) rig.round();
    static_cast<void>(rig.sessions[0].submit([](sim::SeatCommandKey) { return eawr::core::Result<std::vector<std::uint8_t>>::success({0}); }));
    while (rig.now < 33 * 480) rig.round();
    for (const net::PeerIndex seat : {1U, 2U, 3U}) {
        expect(rig.failed[seat].empty(), "survivors stay healthy after the host leaves: " + rig.failed[seat]);
        expect(rig.sessions[seat].timing_frozen(), "with the host departed, timing freezes at the last agreed delay");
        expect(rig.sessions[seat].input_delay() == 4, "no delay change after the host left");
        expect(rig.adapters[seat].tick == 420, "play continues without a host");
    }
}

} // namespace

int main() {
    try {
        const std::vector<std::pair<std::string, std::function<void()>>> repair_cases{
            {"online Start barrier", three_seat_online_start_barrier},
            {"tail overflow", bounded_tail_overflow_uses_departure},
            {"repaired tail", repaired_pair_replays_its_own_tail},
            {"late send", [] { late_noticing_send_failure_uses_the_mesh(); }},
            {"monitored late send", [] { late_noticing_send_failure_uses_the_mesh(true); }},
            {"concurrent departure deadline", ordinary_departure_waits_for_the_incident},
            {"settlement deadline", confirmed_departure_keeps_its_timeout},
            {"quit and repair", repaired_pair_preserves_an_ordinary_quit},
            {"timing and repair", repaired_pair_preserves_pending_timing},
            {"settled vote replay", settled_vote_replay_requires_an_exact_vote},
        };
        for (const auto& [name, run] : repair_cases) {
            std::vector<std::string> reference;
            for (const unsigned workers : {1U, 2U, 4U, 8U}) {
                test_workers = workers; worker_evidence.clear(); run();
                if (workers == 1) reference = worker_evidence;
                else expect(worker_evidence == reference, name + ": tick hashes, outcomes and cutoff ticks match 1 worker at " + std::to_string(workers));
            }
        }
        test_workers = 1; worker_evidence.clear();
        departure_encoding_remains_required_after_setup();
        unsupported_monitor_fails_before_play();
        partitioned_pair_loss();
        no_decider_ends_the_match();
        decider_loss_ends_the_match();
        ordinary_quit_never_starts_an_incident(4);
        ordinary_quit_never_starts_an_incident(3);
        ordinary_timeout_ends_without_an_invented_departure();
        // No seat is all-open: the largest majority clique decides. A crashed seat (all its pairs gone) in a 4-seat mesh:
        cut_scenario("crash of one of four", 4, {{3, 0}, {3, 1}, {3, 2}}, {0, 1, 2}, {{3, net::mesh_errors::no_majority}});
        // Pairs ab, cd and ea lost among five: {b, c, e} beats {b, d, e} by the lowest seat list; b decides.
        cut_scenario("clique of five", 5, {{0, 1}, {2, 3}, {4, 0}}, {1, 2, 4}, {{0, net::mesh_errors::no_majority}, {3, net::mesh_errors::left}});
        pair_loss_proposal_needs_a_decision();
        host_timing_takes_the_per_pair_minimum();
        host_loss_freezes_timing();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) return 1;
    std::cout << "mesh session contracts passed\n";
    return 0;
}
