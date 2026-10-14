#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/tactical_adapter.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "fake_transport.hpp"
#include "diagnostic_sink.hpp"
#include "setup_test_support.hpp"
#if defined(EAWR_TEST_GNS)
#include "eawr/net/gns_transport.hpp"
#include <chrono>
#include <thread>
#endif

#include <array>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
using sim::math::Fixed;
using Bytes = std::vector<std::uint8_t>;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
Fixed units(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

tactical::TacticalSetup setup() {
    tactical::TacticalSetup result;
    result.seed = 714;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    for (std::uint32_t owner = 1; owner <= 2; ++owner) {
        tactical::UnitState unit;
        unit.entity_id = owner; unit.type_id = 1; unit.owner = owner;
        unit.position = {units(owner == 1 ? -50 : 50), {}, {}};
        unit.rotation = owner == 1 ? sim::math::identity_quat() : sim::math::Quat{{}, {}, units(1), {}};
        result.units.push_back(unit);
    }
    return result;
}
tactical::TacticalContent content() {
    tactical::TacticalContent result;
    result.sensors = {{1, units(1000)}};
    result.motion.rules.arc_degrees = units(15);
    result.motion.rules.expansion_distance = units(300);
    result.motion.profiles = {{1, units(2), units(1), units(1), units(5), units(1), {}, {}}};
    tactical::WeaponProfile weapon;
    weapon.range = units(500);
    weapon.min_recharge_hundredths = 20;
    weapon.max_recharge_hundredths = 30;
    weapon.opportunity_when_targeting = true;
    tactical::CombatProfile profile;
    profile.type_id = 1; profile.category_bits = 1; profile.max_attack_distance = units(500);
    profile.weapons = {weapon}; result.combat.profiles = {profile};
    return result;
}
std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    expect(static_cast<bool>(file), "missing diagnostic file: " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Qualification wrapper: reconstruct an equivalent mutable tactical session at
// the injection point, perturb it before step, then replace the delegate. No
// product const view is mutated and no core test hook is needed.
class TestAdapter final : public net::SessionAdapter {
public:
    TestAdapter(tactical::TacticalSetup initial, tactical::TacticalContent rules,
        const sim::PartitionExecutor& executor, std::vector<tactical::SeatBinding> roster,
        std::optional<std::uint64_t> fault)
        : rules_(std::move(rules)), executor_(executor), roster_(std::move(roster)), fault_(fault) {
        delegate_ = std::make_unique<net::TacticalSessionAdapter>(require(tactical::TacticalSession::create(initial, rules_)), executor_, roster_);
    }
    std::uint64_t completed_tick() const override { return delegate_->completed_tick(); }
    std::string state_hash() const override { return delegate_->state_hash(); }
    std::vector<std::uint8_t> canonical_state() const override { return delegate_->canonical_state(); }
    eawr::core::Result<void> validate(const net::SeatInput& input) const override { return delegate_->validate(input); }
    eawr::core::Result<void> install(std::span<const net::SeatInput> input) override { return delegate_->install(input); }
    eawr::core::Result<std::vector<std::uint8_t>> canonical_input() const override { return delegate_->canonical_input(); }
    bool is_departure(const net::SeatInput& input) const override { return delegate_->is_departure(input); }
    eawr::core::Result<net::CompletedTick> step() override {
        if (fault_ && *fault_ > 0 && completed_tick() + 1 == *fault_) {
            auto restored = require(tactical::TacticalSession::from_replay(simulation().record(), rules_));
            while (restored.completed_tick() < completed_tick()) require(restored.step(executor_));
            expect(restored.canonical_state_bytes() == canonical_state(), "fault wrapper did not reconstruct equivalent state");
            require(restored.stage_remove(2));
            delegate_ = std::make_unique<net::TacticalSessionAdapter>(std::move(restored), executor_, roster_);
            fault_.reset();
        }
        return delegate_->step();
    }
    const tactical::TacticalSession& simulation() const { return delegate_->simulation(); }
    void inject_at(std::uint64_t tick) const { fault_ = tick; }
private:
    tactical::TacticalContent rules_;
    const sim::PartitionExecutor& executor_;
    std::vector<tactical::SeatBinding> roster_;
    mutable std::optional<std::uint64_t> fault_;
    std::unique_ptr<net::TacticalSessionAdapter> delegate_;
};

std::string run_case(const std::filesystem::path& directory, std::uint64_t final_tick,
    std::optional<std::uint64_t> fault_tick = {}, std::optional<eawr::testing::WanProfile> wan = {}) {
    sim::InlineExecutor executor;
#if defined(EAWR_TEST_GNS)
    expect(!wan, "WAN emulation requires the fake transport");
    std::array<std::unique_ptr<net::GnsTransport>, 2> transports;
    net::GnsOptions transport_options; transport_options.build_identity = "gns-desync-test";
    transports[0] = require(net::GnsTransport::host("127.0.0.1", 0, transport_options));
    transports[1] = require(net::GnsTransport::connect("127.0.0.1", transports[0]->port(), transport_options));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (transports[0]->link(1) != net::LinkState::open || transports[1]->link(0) != net::LinkState::open) {
        static_cast<void>(transports[0]->poll()); static_cast<void>(transports[1]->poll());
        expect(std::chrono::steady_clock::now() < deadline, "GNS desync handshake deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#else
    eawr::testing::FakeNetwork network(2);
    if (wan) network.emulate(*wan);
    std::array<std::unique_ptr<eawr::testing::FakeTransport>, 2> transports;
#endif
    std::array<std::unique_ptr<net::DesyncMonitor>, 2> monitors;
    std::vector<std::unique_ptr<TestAdapter>> adapters;
    std::vector<net::LockstepSession> sessions;
    const std::vector<tactical::SeatBinding> roster{{sim::SeatId{1}, 1, true, false}, {sim::SeatId{2}, 2, true, false}};
    net::SessionOptions session_options;
    session_options.input_delay = 3;
    session_options.final_tick_count = final_tick;
    session_options.completeness_timeout_ms = 100'000;
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        auto initial = setup();
        if (peer == 1 && fault_tick == 0) ++initial.seed;
#if !defined(EAWR_TEST_GNS)
        transports[peer] = std::make_unique<eawr::testing::FakeTransport>(network, peer);
#endif
        net::DesyncOptions options;
        options.diagnostic_sink = eawr::testing::platform_diagnostic_sink(directory);
        options.match_identity = "scripted-battle";
        options.match_setup = require(tactical::write_replay({initial, 0, {}}));
        options.build_identity = "contract-test-build";
        monitors[peer] = std::make_unique<net::DesyncMonitor>(*transports[peer], std::move(options));
        adapters.push_back(std::make_unique<TestAdapter>(initial, content(), executor, roster, peer == 1 ? fault_tick : std::nullopt));
        sessions.push_back(require(net::LockstepSession::create(*adapters.back(), *transports[peer],
            {{sim::SeatId{1}, true}, {sim::SeatId{2}, true}}, session_options, monitors[peer].get())));
    }
    // Hold some complete ordered streams. Exchange latency must only gate stepping,
    // never turn an absent row into an agreement or stop transport service.
#if !defined(EAWR_TEST_GNS)
    network.fault = [](auto from, auto to, auto frame) {
        eawr::testing::FakeDelivery delivery;
        if (from == 1 && to == 0 && !frame.empty() && frame[0] == net::desync_combined_kind)
            delivery.ordered_delay = 3;
        return delivery;
    };
#endif
    std::array<unsigned, 2> script{};
    bool completed = false;
    for (std::uint64_t round = 0; round < (wan ? 1000000U : 12000U); ++round) {
#if defined(EAWR_TEST_GNS)
        expect(std::chrono::steady_clock::now() < deadline, "GNS desync battle deadline");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#else
        network.round = round;
#endif
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            auto& session = sessions[peer];
            const auto tick = adapters[peer]->completed_tick();
            const auto submit = [&](const net::LocalIntent& intent) {
                return session.submit([&](sim::SeatCommandKey key) { return net::encode_tactical_input(key, roster[peer], intent); });
            };
            if (!fault_tick && tick < final_tick - 3 && !monitors[peer]->frozen()) {
                const auto owner = peer + 1;
                if (script[peer] == 0) {
                    require(submit({{owner}, tactical::AttackPayload{owner == 1 ? 2U : 1U}}));
                    ++script[peer];
                } else if (script[peer] == 1 && tick >= 15) {
                    require(submit({{owner}, tactical::MovePayload{{units(owner == 1 ? -20 : 20), units(10), {}}}}));
                    ++script[peer];
                } else if (script[peer] == 2 && tick >= 40) {
                    require(submit({{owner}, tactical::AttackPayload{owner == 1 ? 2U : 1U}}));
                    ++script[peer];
                }
            }
            require(session.pump(round));
        }
        completed = fault_tick ? monitors[0]->report().has_value() && monitors[1]->report().has_value()
            : monitors[0]->agreed_end() && monitors[1]->agreed_end();
        if (completed) break;
    }
    expect(completed, "desync session did not complete its diagnostic exchange");
    for (const auto& monitor : monitors)
        expect(monitor->setup_agreed() == (fault_tick != 0), "wrong setup barrier agreement status");
    if (!fault_tick) {
        expect(adapters[0]->completed_tick() == 2000, "scripted battle did not run 2000 ticks");
        expect(adapters[0]->state_hash() == adapters[1]->state_hash(), "battle final hashes differ");
        expect(adapters[0]->simulation().record() == adapters[1]->simulation().record(), "battle canonical input histories differ");
        expect(adapters[0]->simulation().record().commands.size() == 6, "scripted input was not executed");
        expect(!monitors[0]->report() && !monitors[1]->report(), "false positive in scripted battle");
        return adapters[0]->state_hash();
    }
    const auto detected = std::min(final_tick, ((*fault_tick + 29) / 30) * 30);
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        const auto& monitor = *monitors[peer];
        const auto& report = *monitor.report();
        expect(monitor.frozen() && !monitor.can_step(), "desync did not freeze simulation");
        expect(report.first_divergent_tick == *fault_tick && report.detected_at == detected, "wrong first divergent tick");
        expect(adapters[peer]->completed_tick() == detected, "peer stepped beyond mismatch frontier");
        expect(!report.bundle_error, "bundle could not be written");
        auto initial = setup();
        if (peer == 1 && fault_tick == 0) ++initial.seed;
        const std::filesystem::path location = report.bundle_location;
        expect(!location.empty(), "sink returned no diagnostic location");
        expect(read_bytes(location / "setup.bin") == require(tactical::write_replay({initial, 0, {}})), "bundle setup bytes differ");
        const auto encoding = read_bytes(location / "session.bin");
        expect(!encoding.empty(), "bundle canonical encoding is empty");
        if (report.encoding_tick == detected)
            expect(encoding == adapters[peer]->canonical_state(), "bundle canonical encoding differs");
        for (const auto* name : {"report.txt", "inputs.bin", "identity.txt", "hashes.csv", "README.txt"})
            expect(!read_bytes(location / name).empty(), std::string("bundle file empty: ") + name);
        for (const auto& file : report.bundle.files)
            expect(read_bytes(location / file.name) == file.bytes, "platform publication differs from memory bundle");
    }
    expect(monitors[0]->report()->local_hash == monitors[1]->report()->remote_hash, "peers report different hash pairs");
    if (detected + session_options.input_delay < final_tick) {
        require(sessions[0].submit([&](sim::SeatCommandKey key) {
            return net::encode_tactical_input(key, roster[0], {{1}, tactical::MovePayload{{units(-10), {}, {}}}});
        }));
        expect(!sessions[0].failure(), "non-opt-in frozen admission became a terminal error");
    }
    // Verify that pump still routes other traffic without advancing a frozen world.
    bool serviced = false;
    sessions[1].set_message_handler([&](net::Received frame) {
        serviced = frame.from == 0 && frame.frame == std::vector<std::uint8_t>{40};
        return eawr::core::Result<void>::success();
    });
    require(transports[0]->send(1, std::array<std::uint8_t, 1>{40}));
    for (unsigned extra = 0; extra < (wan ? 2000U : 10U); ++extra) {
#if defined(EAWR_TEST_GNS)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        for (auto& session : sessions) require(session.pump(12000 + extra));
#else
        ++network.round;
        for (auto& session : sessions) require(session.pump(network.round));
#endif
    }
    expect(serviced, "frozen session stopped servicing transport");
    for (const auto& adapter : adapters) expect(adapter->completed_tick() == detected, "frozen session resumed stepping");
    return adapters[0]->state_hash();
}
struct ResyncMatch {
    ResyncMatch(std::size_t workers, std::uint32_t peers, std::optional<std::uint64_t> fault,
        net::PeerIndex minority, bool persistent = false, bool conflicting_log = false,
        std::optional<bool> adaptive_decrease = {}, bool allow_rejoin = true)
        : network(peers), executor(workers), bad_peer(minority) {
        for (net::PeerIndex peer = 0; peer < peers; ++peer) {
            roster.push_back({sim::SeatId{peer + 1}, peer % 2 + 1, true, false});
            seats.push_back({sim::SeatId{peer + 1}, true});
            std::array<std::uint8_t, 32> token{}; token.fill(static_cast<std::uint8_t>(peer + 1)); tokens.push_back(token);
        }
        for (net::PeerIndex peer = 0; peer < peers; ++peer) {
            transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, peer));
            net::DesyncOptions diagnostic; diagnostic.exchange_every = 5;
            monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), diagnostic));
            adapters.push_back(std::make_unique<TestAdapter>(setup(), content(), executor, roster, peer == minority ? fault : std::nullopt));
            net::SessionOptions options; options.final_tick_count = adaptive_decrease ? 360U : 90U;
            final_tick = options.final_tick_count;
            if (adaptive_decrease) {
                options.input_delay = *adaptive_decrease ? 8U : 4U;
                options.non_lan = true;
                options.adaptive_timing = net::AdaptiveTiming{0, true, [decrease = *adaptive_decrease](auto) {
                    return std::optional<std::uint64_t>{decrease ? 0U : 330U};
                }};
            }
            options.reconnect = net::ReconnectOptions{};
            options.reconnect->match_identity.fill(42); options.reconnect->setup = require(tactical::write_replay({setup(), 0, {}}));
            options.reconnect->allow_rejoin = allow_rejoin;
            if (allow_rejoin) options.reconnect->seat_tokens = setup_test::pair_keys(peer, peers);
            options.reconnect->replay_control = [](auto) { return eawr::core::Result<void>::success(); };
            options.resync = net::ResyncOptions{}; options.resync->timeout_ms = 100;
            options.resync->rebuild = [this, persistent, fault](auto bytes) {
                expect(Bytes(bytes.begin(), bytes.end()) == require(tactical::write_replay({setup(), 0, {}})), "factory received unverified setup");
                ++rebuilds;
                std::unique_ptr<net::SessionAdapter> fresh = std::make_unique<TestAdapter>(setup(), content(), executor, roster,
                    persistent ? fault : std::nullopt);
                return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(fresh));
            };
            options.resync->replay_control = [this](net::SessionAdapter& staging, auto bytes) {
                expect(staging.completed_tick() == 3 && bytes.size() == control.size()
                    && std::equal(bytes.begin(), bytes.end(), control.begin()), "custom control lost its original replay boundary");
                ++replayed_controls;
                return eawr::core::Result<void>::success();
            };
            if (peer == 0) {
                const auto unsupported = net::TacticalLockstepSession::create(setup(), content(), *transports.back(), executor,
                    roster, options, monitors.back().get());
                expect(!unsupported && unsupported.error().code == net::resync_errors::options,
                    "typed convenience owner silently retained an obsolete adapter");
            }
            sessions.push_back(require(net::LockstepSession::create(*adapters.back(), *transports.back(), seats, options, monitors.back().get())));
            const Bytes retained{static_cast<std::uint8_t>(conflicting_log && peer == minority ? 8 : 7)};
            require(sessions.back().retain_reconnect_control(3, retained));
        }
        for (net::PeerIndex peer = 0; peer < peers; ++peer)
            require(sessions[peer].submit([&, peer](sim::SeatCommandKey key) {
                return net::encode_tactical_input(key, roster[peer], {{roster[peer].player},
                    tactical::MovePayload{{units(roster[peer].player == 1 ? -20 : 20), units(10), {}}}});
            }));
    }
    void pump() {
        network.round = now;
        for (auto& session : sessions) if (!session.failure()) {
            const auto result = session.pump(now);
            if (!result) errors.push_back(result.error().code);
        }
        ++now;
    }
    bool ended() const {
        return std::all_of(sessions.begin(), sessions.end(), [this](const auto& session) { return session.active_adapter().completed_tick() == final_tick; })
            && std::all_of(monitors.begin(), monitors.end(), [](const auto& monitor) { return monitor->agreed_end(); });
    }
    bool failed() const {
        return std::all_of(sessions.begin(), sessions.end(), [](const auto& session) { return session.failure().has_value(); });
    }
    eawr::testing::FakeNetwork network;
    eawr::platform::ThreadWorkerAdapter executor;
    net::PeerIndex bad_peer;
    std::uint64_t now{};
    std::uint64_t final_tick{90};
    unsigned rebuilds{}, replayed_controls{};
    Bytes control{7};
    std::vector<std::array<std::uint8_t, 32>> tokens;
    std::vector<tactical::SeatBinding> roster;
    std::vector<net::PeerSeat> seats;
    std::vector<std::unique_ptr<eawr::testing::FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<std::unique_ptr<TestAdapter>> adapters;
    std::vector<net::LockstepSession> sessions;
    std::vector<std::string> errors;
};

std::string resync_success(std::size_t workers, std::uint64_t tick, std::uint32_t peers = 2, net::PeerIndex minority = 1,
    std::optional<bool> adaptive_decrease = {}, bool allow_rejoin = true) {
    ResyncMatch uninterrupted(workers, peers, {}, minority, false, false, adaptive_decrease, allow_rejoin),
        match(workers, peers, tick, minority, false, false, adaptive_decrease, allow_rejoin);
    // The third vote is delayed while another stream delivers source traffic.
    match.network.fault = [minority](auto from, auto to, auto bytes) {
        eawr::testing::FakeDelivery delivery;
        if (from == 2 && to == minority && bytes.front() == net::resync_vote_kind) delivery.ordered_delay = 8;
        if (bytes.front() == net::resync_ready_kind) delivery.ordered_delay = from == 0 ? 3 : 1;
        return delivery;
    };
    for (unsigned round = 0; round < 1000 && (!match.ended() || !uninterrupted.ended()); ++round) {
        match.pump(); uninterrupted.pump();
    }
    expect(match.errors.empty() && match.ended() && uninterrupted.ended(), "resync did not resume to the final barrier");
    expect(match.rebuilds == 1 && match.replayed_controls == 1, "resync did not rebuild exactly the diverged seat once");
    for (net::PeerIndex peer = 0; peer < peers; ++peer) {
        expect(match.sessions[peer].active_adapter().canonical_state() == uninterrupted.sessions[peer].active_adapter().canonical_state(),
            "resync differs from uninterrupted authoritative state");
        const auto& repaired = static_cast<const TestAdapter&>(match.sessions[peer].active_adapter());
        const auto& original = static_cast<const TestAdapter&>(uninterrupted.sessions[peer].active_adapter());
        expect(repaired.simulation().record() == original.simulation().record(), "resync lost or duplicated replay inputs");
        expect(match.monitors[peer]->recovered_report() && !match.monitors[peer]->recovered_report()->bundle.files.empty(),
            "successful recovery discarded its diagnostics");
        expect(match.monitors[peer]->agreed_end(), "resync lost final monitor agreement");
        expect(match.sessions[peer].input_delay() == uninterrupted.sessions[peer].input_delay()
            && match.sessions[peer].input_horizon() == uninterrupted.sessions[peer].input_horizon(), "resync lost replayed timing policy");
    }
    expect(&match.sessions[minority].active_adapter() != match.adapters[minority].get(), "diverged adapter was not swapped");
    const auto source = minority == 0 ? 1U : 0U;
    expect(&match.sessions[source].active_adapter() == match.adapters[source].get(), "authoritative adapter was replaced unnecessarily");
    const auto hash = match.sessions[0].active_adapter().state_hash();
    std::cout << "resync workers=" << workers << " fault=" << tick << " peers=" << peers << " state=" << hash << '\n';
    return hash;
}

void resync_new_incident() {
    ResyncMatch match(2, 2, 7, 1);
    while (match.sessions[1].active_adapter().completed_tick() < 20 && match.errors.empty()) match.pump();
    static_cast<const TestAdapter&>(match.sessions[1].active_adapter()).inject_at(31);
    for (unsigned round = 0; round < 500 && !match.ended() && match.errors.empty(); ++round) match.pump();
    expect(match.errors.empty() && match.ended() && match.rebuilds == 2, "separate incident did not get exactly one new attempt");
    expect(match.monitors[1]->recovered_report()->first_divergent_tick == 31, "second incident reused old diagnostic rows");
    expect(match.sessions[0].active_adapter().state_hash() == match.sessions[1].active_adapter().state_hash(), "second recovery differs");
}

void resync_tie_unreproducible() {
    ResyncMatch match(2, 2, 7, 0);
    for (unsigned round = 0; round < 500 && !match.failed(); ++round) match.pump();
    expect(match.failed() && match.rebuilds == 1
        && std::find(match.errors.begin(), match.errors.end(), net::resync_errors::hash) != match.errors.end(),
        "two-peer tie must choose lowest seat and fail if that state cannot be replayed");
    for (net::PeerIndex peer = 0; peer < 2; ++peer)
        expect(&match.sessions[peer].active_adapter() == match.adapters[peer].get(), "unreproducible tie published staging");
}

void resync_failure(bool conflicting_log = false, bool stall = false, bool corrupt = false) {
    ResyncMatch match(2, 2, 7, 1, !conflicting_log && !stall && !corrupt, conflicting_log);
    if (stall) match.network.fault = [](auto, auto, auto bytes) {
        eawr::testing::FakeDelivery delivery; delivery.drop = bytes.front() == net::resync_proof_kind; return delivery;
    };
    bool corrupted{};
    for (unsigned round = 0; round < 500 && !match.failed(); ++round) {
        match.pump();
        if (corrupt && !corrupted) for (auto& inbox : match.network.inbox) for (auto& frame : inbox)
            if (frame.received.frame.front() == net::resync_chunk_kind) { frame.received.frame.back() ^= 1; corrupted = true; }
    }
    expect(match.failed(), "failed resync did not terminate all peers");
    expect(match.rebuilds <= 1, "failed incident retried recovery");
    const auto expected = conflicting_log ? net::resync_errors::agreement : stall ? net::resync_errors::timeout
        : corrupt ? net::resync_errors::replay : net::resync_errors::hash;
    expect(std::find(match.errors.begin(), match.errors.end(), expected) != match.errors.end(), "wrong recovery failure diagnostic");
    for (net::PeerIndex peer = 0; peer < 2; ++peer) {
        expect(&match.sessions[peer].active_adapter() == match.adapters[peer].get(), "failed staging published an adapter");
        expect(match.monitors[peer]->frozen() && match.monitors[peer]->report(), "failed recovery discarded frozen diagnostics");
        expect(match.sessions[peer].active_adapter().completed_tick() == 10, "failed recovery advanced the live world");
    }
}

void resync_departure_mid_incident() {
    ResyncMatch match(2, 3, 7, 1);
    for (auto& session : match.sessions) session.enable_departures([&](sim::SeatCommandKey key) {
        const auto found = std::find_if(match.roster.begin(), match.roster.end(), [&](const auto& value) { return value.seat == key.seat; });
        return net::encode_tactical_input(key, *found, {{}, tactical::QuitPayload{}});
    });
    bool sent{};
    for (unsigned round = 0; round < 500 && !match.failed(); ++round) {
        match.pump();
        if (!sent && match.sessions[0].resyncing()) {
            Bytes proposal{net::departure_proposal_kind};
            const auto append = [&](std::uint64_t value, unsigned width) {
                for (unsigned index = 0; index < width; ++index) proposal.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
            };
            append(1, 4); append(static_cast<std::uint8_t>(net::DepartureReason::timeout), 1);
            append(match.sessions[0].active_adapter().completed_tick(), 8);
            require(match.transports[2]->send(0, proposal));
            sent = true;
        }
    }
    expect(sent && match.failed() && match.rebuilds == 0, "remote departure reopened the resync incident");
    for (net::PeerIndex peer = 0; peer < 3; ++peer) {
        expect(match.sessions[peer].failure()->code == net::resync_errors::overlap, "overlap did not propagate the agreed refusal");
        expect(match.monitors[peer]->frozen() && match.monitors[peer]->report()
            && !match.monitors[peer]->report()->diagnostic_only, "departure cleared or reclassified the desync diagnosis");
        expect(&match.sessions[peer].active_adapter() == match.adapters[peer].get(), "departure race published staging");
    }
}

void resync_ready_crosses_deadline(bool lost_link = false) {
    ResyncMatch match(2, 3, 7, 1);
    std::optional<std::uint64_t> committed_at;
    match.network.before_send = [&](auto from, auto, auto bytes) {
        if (from == 0 && bytes.front() == net::resync_ready_kind && !committed_at) committed_at = match.now;
    };
    match.network.fault = [](auto from, auto to, auto bytes) {
        eawr::testing::FakeDelivery action;
        if (from != 0 && to == 0 && bytes.front() == net::resync_ready_kind) action.ordered_delay = 120;
        return action;
    };
    bool waited_past_deadline{};
    for (unsigned round = 0; round < 1000 && !match.ended() && !match.failed(); ++round) {
        if (lost_link && committed_at && match.now > *committed_at + 5)
            std::fill(match.network.open.begin(), match.network.open.end(), false);
        match.pump();
        if (match.now > 110 && match.sessions[0].resyncing() && !match.sessions[0].failure()) waited_past_deadline = true;
    }
    expect(committed_at.has_value(), "fixture never committed READY");
    if (lost_link) {
        expect(match.failed(), "committed recovery ignored closed voter streams");
        expect(match.sessions[0].failure()->code == net::resync_errors::overlap,
            "committed waiter ignored closed voter links");
        for (const auto& session : match.sessions) expect(session.failure()->code == net::resync_errors::overlap
            || session.failure()->code == net::session_errors::link_closed, "closed link did not end all peers");
    } else {
        expect(waited_past_deadline && match.errors.empty() && match.ended(), "READY delivery across the deadline split the outcome");
        for (const auto& session : match.sessions) expect(session.active_adapter().state_hash() == match.sessions[0].active_adapter().state_hash(),
            "late READY recovery hashes differ");
    }
}

void resync_hold_overlap() {
    ResyncMatch match(2, 3, 4, 1);
    for (auto& session : match.sessions) session.enable_departures([&](sim::SeatCommandKey key) {
        const auto found = std::find_if(match.roster.begin(), match.roster.end(), [&](const auto& binding) { return binding.seat == key.seat; });
        return net::encode_tactical_input(key, *found, {{}, tactical::QuitPayload{}});
    });
    for (unsigned round = 0; round < 500; ++round) {
        if (match.sessions[0].active_adapter().completed_tick() >= 5) match.network.open[2] = false;
        match.network.round = match.now;
        for (net::PeerIndex peer = 0; peer < 3; ++peer) {
            if (!match.network.open[peer] || match.sessions[peer].failure()) continue;
            const auto result = match.sessions[peer].pump(match.now);
            if (!result) match.errors.push_back(result.error().code);
        }
        ++match.now;
        if (match.sessions[0].failure() || match.sessions[1].failure()) break;
    }
    expect(std::find(match.errors.begin(), match.errors.end(), net::resync_errors::overlap) != match.errors.end()
        && match.rebuilds == 0, "reconnect/departure overlap attempted resync");
}

} // namespace

int main(int argc, char** argv) {
    try {
#if defined(EAWR_TEST_GNS)
        if (!net::GnsTransport::available()) { std::cout << "GNS disabled: desync test skipped\n"; return 77; }
#endif
        expect(argc == 2, "usage: desync_session_tests artifact-directory");
        [[maybe_unused]] const auto baseline = run_case(argv[1], 2000);
        run_case(argv[1], 90, 48);
        run_case(argv[1], 90, 0);
        run_case(argv[1], 55, 48);
        std::string recovered_hash;
        for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto tick : {1U, 7U, 29U, 31U}) {
            const auto hash = resync_success(workers, tick);
            if (recovered_hash.empty()) recovered_hash = hash;
            expect(hash == recovered_hash, "resync differs on 1/2/4/8 workers");
        }
        resync_success(4, 7, 3, 0); // Corrupt the lowest seat: the majority must win.
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            resync_success(workers, 140, 2, 1, false); // Already agreed future increase.
            resync_success(workers, 190, 2, 1, false); // Active increase replayed.
            resync_success(workers, 320, 2, 1, true); // Active decrease replayed.
        }
        for (const auto workers : {1U, 2U, 4U, 8U}) resync_success(workers, 7, 2, 1, {}, false);
        resync_new_incident();
        resync_tie_unreproducible();
        resync_failure();
        resync_failure(true);
        resync_failure(false, true);
        resync_failure(false, false, true);
        resync_hold_overlap();
        resync_departure_mid_incident();
        resync_ready_crosses_deadline();
        resync_ready_crosses_deadline(true);
#if !defined(EAWR_TEST_GNS)
        for (const auto& profile : eawr::testing::wan_profiles) {
            const auto directory = std::filesystem::path(argv[1]) / profile.name;
            expect(run_case(directory, 2000, {}, profile) == baseline, "WAN monitor retains baseline state hash");
            run_case(directory, 90, 48, profile);
        }
#endif
        std::cout << "desync session integration contracts passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
