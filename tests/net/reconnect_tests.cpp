#include "../support/check.hpp"
#include "../support/result_require.hpp"
#include "eawr/net/tactical_adapter.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "fake_transport.hpp"
#include "setup_test_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
using Bytes = std::vector<std::uint8_t>;
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool ok, const std::string& message) {
    if (!ok) local_checker.check_before_print(ok, message, std::cerr, "FAIL: ");
}
using eawr::testing::result_code_colon::require;

sim::math::Fixed units(std::int64_t value) { return sim::math::Fixed::from_raw(value * sim::math::Fixed::scale); }
tactical::TacticalSetup setup(std::uint32_t players = 2) {
    tactical::TacticalSetup result;
    result.seed = 714;
    result.departure_policy = tactical::MultiplayerDeparturePolicy{};
    for (std::uint32_t player = 1; player <= players; ++player) {
        result.players.push_back({player, player, player, tactical::player_flag_commandable});
        result.departure_policy->humans.push_back(player);
        tactical::UnitState unit;
        unit.entity_id = player; unit.type_id = 1; unit.owner = player;
        unit.position = {units(player == 1 ? -50 : 50 * static_cast<std::int64_t>(player - 1)), {}, {}};
        unit.rotation = sim::math::identity_quat();
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
    return result;
}
struct Match {
    explicit Match(std::size_t workers, std::uint32_t peers = 2, bool monitored = false, std::uint64_t grace = 200,
        std::uint64_t final_tick = 50, const std::function<void(net::SessionOptions&)>& configure = {}, bool distinct_players = false)
        : network(peers), executor(workers), monitoring(monitored) {
        player_count = distinct_players ? peers : 2;
        options.final_tick_count = final_tick; options.completeness_timeout_ms = 100;
        net::ReconnectOptions reconnect;
        reconnect.match_identity.fill(42); reconnect.grace_ms = grace; reconnect.setup = {1, 2, 3};
        reconnect.progress_timeout_ms = grace; reconnect.hello_retry_ms = 2;
        for (std::uint32_t peer = 0; peer < peers; ++peer) {
            peer_keys.push_back(setup_test::pair_keys(peer, peers));
            std::array<std::uint8_t, 32> token{}; token.fill(static_cast<std::uint8_t>(peer + 1));
            reconnect.seat_tokens.push_back(token);
            bindings.push_back({sim::SeatId{peer + 1}, distinct_players ? peer + 1 : peer % 2 + 1, true, false});
            seats.push_back({sim::SeatId{peer + 1}, true});
            transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, peer));
        }
        reconnect.seat_tokens = setup_test::pair_keys(0, peers);
        options.reconnect = reconnect;
        if (configure) configure(options);
        for (net::PeerIndex peer = 0; peer < peers; ++peer) replace(peer, false);
    }
    void replace(net::PeerIndex peer, bool joining, std::uint64_t epoch = 1) {
        const auto peers = network.inbox.size();
        if (adapters.size() < peers) { adapters.resize(peers); sessions.resize(peers); monitors.resize(peers); controls.resize(peers); }
        sessions[peer].reset(); monitors[peer].reset();
        adapters[peer] = std::make_unique<net::TacticalSessionAdapter>(
            require(tactical::TacticalSession::create(setup(player_count), content())), executor, bindings);
        net::SessionOptions config{};
        config = options;
        if (config.reconnect->allow_rejoin) config.reconnect->seat_tokens = peer_keys[peer];
        config.reconnect->replay_control = [this, peer](std::span<const std::uint8_t> bytes) {
            controls[peer].push_back({adapters[peer]->completed_tick(), Bytes(bytes.begin(), bytes.end())});
            return eawr::core::Result<void>::success();
        };
        // Adaptive timing has a core-owned binding; callers need no self-capturing callback.
        if (config.adaptive_timing) config.reconnect->replay_control = {};
        if (joining) { config.reconnect->rejoin_source = peer == 0 ? 1U : 0U; config.reconnect->rejoin_epoch = epoch; }
        if (monitoring) {
            net::DesyncOptions monitor_options; monitor_options.exchange_every = 5;
            monitors[peer] = std::make_unique<net::DesyncMonitor>(*transports[peer], monitor_options);
        }
        sessions[peer] = std::make_unique<net::LockstepSession>(require(net::LockstepSession::create(
            *adapters[peer], *transports[peer], seats, config, monitors[peer].get())));
        sessions[peer]->enable_departures([this](sim::SeatCommandKey key) {
            const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) { return binding.seat == key.seat; });
            return net::encode_tactical_input(key, *found, {{}, tactical::QuitPayload{}});
        });
    }
    void submit(net::PeerIndex peer) {
        require(sessions[peer]->submit([&](sim::SeatCommandKey key) {
            const auto player = bindings[peer].player;
            return net::encode_tactical_input(key, bindings[peer], {{player},
                tactical::MovePayload{{units(player == 1 ? -20 : 20), units(10), {}}}});
        }));
    }
    void quit(net::PeerIndex peer) {
        require(sessions[peer]->submit([&](sim::SeatCommandKey key) {
            return net::encode_tactical_input(key, bindings[peer], {{}, tactical::QuitPayload{}});
        }));
    }
    void pump(std::optional<net::PeerIndex> absent = {}) {
        network.round = now;
        for (net::PeerIndex peer = 0; peer < sessions.size(); ++peer) {
            if ((absent && *absent == peer) || !network.open[peer]) continue;
            require(sessions[peer]->pump(now));
        }
        ++now;
    }
    void until(std::uint64_t tick) {
        for (unsigned index = 0; index < 2000; ++index) {
            if (std::all_of(adapters.begin(), adapters.end(), [tick](const auto& adapter) { return adapter->completed_tick() >= tick; })) return;
            pump();
        }
        throw std::runtime_error("match did not reach tick " + std::to_string(tick));
    }
    void hold(net::PeerIndex peer) {
        network.open[peer] = false;
        for (unsigned index = 0; index < 100; ++index) {
            pump(peer);
            bool held = true;
            for (net::PeerIndex survivor = 0; survivor < sessions.size(); ++survivor) {
                if (survivor == peer || !network.open[survivor]) continue;
                const auto status = sessions[survivor]->reconnect_status();
                held = held && status.phase == net::ReconnectPhase::held && adapters[survivor]->completed_tick() == status.barrier_tick;
            }
            if (held) return;
        }
        throw std::runtime_error("survivors did not hold a common barrier");
    }
    eawr::testing::FakeNetwork network;
    eawr::platform::ThreadWorkerAdapter executor;
    bool monitoring{};
    std::uint32_t player_count{2};
    std::uint64_t now{};
    net::SessionOptions options;
    std::vector<net::ReconnectTokens> peer_keys;
    std::vector<tactical::SeatBinding> bindings;
    std::vector<net::PeerSeat> seats;
    std::vector<std::unique_ptr<eawr::testing::FakeTransport>> transports;
    std::vector<std::unique_ptr<net::TacticalSessionAdapter>> adapters;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<std::unique_ptr<net::LockstepSession>> sessions;
    std::vector<std::vector<std::pair<std::uint64_t, Bytes>>> controls;
};

bool connection_lost(const Match& match, net::PeerIndex peer, const eawr::core::Result<net::PumpResult>& result) {
    if (result) return false;
    expect(result.error().code == net::session_errors::stalled
        && result.error().message.starts_with("Connection lost: the match could not continue"),
        "unconfirmed loss has a typed connection-loss reason");
    expect(match.adapters[peer]->simulation().departure_controls().empty() && !match.adapters[peer]->simulation().outcome(),
        "unconfirmed loss cannot invent a departure or award a winner");
    return true;
}

void connecting_rejoin(bool expire) {
    Match match(1);
    match.until(8); match.hold(1);
    match.network.inbox[1].clear(); match.network.open[1] = true;
    match.replace(1, true);
    class ConnectingTransport final : public net::MessageTransport {
    public:
        explicit ConnectingTransport(net::MessageTransport& stream) : stream_(stream) {}
        bool ready{};
        unsigned attempted{};
        eawr::core::Result<void> send(net::PeerIndex to, std::span<const std::uint8_t> bytes) override {
            ++attempted;
            if (!ready) throw std::runtime_error("session sent before replacement authentication");
            return stream_.send(to, bytes);
        }
        std::optional<net::Received> poll() override { return ready ? stream_.poll() : std::nullopt; }
        net::LinkState link(net::PeerIndex peer) const override {
            return peer == self() || ready ? stream_.link(peer) : net::LinkState::connecting;
        }
        net::PeerIndex self() const override { return stream_.self(); }
        std::uint32_t peer_count() const override { return stream_.peer_count(); }
    private:
        net::MessageTransport& stream_;
    } transport(*match.transports[1]);
    auto config = match.options;
    config.reconnect->seat_tokens = match.peer_keys[1];
    config.reconnect->rejoin_source = 0; config.reconnect->rejoin_epoch = 1;
    match.sessions[1] = std::make_unique<net::LockstepSession>(require(net::LockstepSession::create(
        *match.adapters[1], transport, match.seats, config)));
    const auto began = match.now;
    for (unsigned count = 0; count < 20; ++count) match.pump();
    expect(transport.attempted == 0 && match.adapters[1]->completed_tick() == 0,
        "connecting replacement retains a held tick-zero returner without consuming HELLO attempts");
    if (expire) {
        auto result = match.sessions[1]->pump(began + config.reconnect->grace_ms);
        expect(!result && result.error().code == net::reconnect_errors::timeout,
            "replacement authentication wait is bounded by the agreed grace");
    } else {
        transport.ready = true; match.until(50);
        expect(transport.attempted > 0 && match.adapters[1]->state_hash() == match.adapters[0]->state_hash(),
            "authenticated replacement starts HELLO and resumes equal worlds");
    }
}

std::string reconnect(std::size_t workers, std::uint64_t drop_tick, std::uint32_t peers, bool monitor) {
    Match control(workers, peers, monitor), match(workers, peers, monitor);
    for (net::PeerIndex peer = 0; peer < peers; ++peer) { control.submit(peer); match.submit(peer); }
    control.until(50);
    // A tick-zero drop still retains the batches sealed before the link broke.
    match.pump();
    match.until(drop_tick);
    const auto target = peers - 1;
    match.hold(target);
    const auto barrier = match.sessions[0]->reconnect_status().barrier_tick;
    for (unsigned round = 0; round < 5; ++round) match.pump(target);
    expect(match.adapters[0]->completed_tick() == barrier, "held seat pauses simulation");
    expect(!match.sessions[0]->submit([](auto) { return eawr::core::Result<Bytes>::success({1}); }), "held seat rejects new intent");
    match.network.inbox[target].clear(); match.network.open[target] = true;
    match.replace(target, true);
    match.until(50);
    for (const auto& adapter : match.adapters) {
        expect(adapter->state_hash() == control.adapters[0]->state_hash(), "reconnect equals uninterrupted state");
        expect(adapter->simulation().record() == control.adapters[0]->simulation().record(), "reconnect preserves exact replay");
    }
    expect(match.sessions[target]->reconnect_status().phase == net::ReconnectPhase::playing, "returning seat activates after proof");
    return match.adapters[0]->state_hash();
}

std::string adaptive_reconnect(std::size_t workers, bool decrease, std::uint64_t drop_tick, bool monitored = false) {
    const auto configure = [decrease](net::SessionOptions& options) {
        options.input_delay = decrease ? 8U : 4U;
        options.non_lan = true;
        options.adaptive_timing = net::AdaptiveTiming{0, true, [decrease](auto) {
            return std::optional<std::uint64_t>{decrease ? 0U : 330U};
        }};
    };
    Match control(workers, 2, monitored, 1000, 360, configure);
    Match match(workers, 2, monitored, 1000, 360, configure);
    for (unsigned peer = 0; peer < 2; ++peer) { control.submit(peer); match.submit(peer); }
    control.until(drop_tick); match.until(drop_tick);
    for (unsigned peer = 0; peer < 2; ++peer) { control.submit(peer); match.submit(peer); }
    // Seal the second command wave before replacing the stream.
    control.pump(); match.pump();
    control.until(360);
    match.hold(1);
    const auto barrier = match.sessions[0]->reconnect_status().barrier_tick;
    expect(barrier > drop_tick && barrier < 360, "adaptive reconnect holds a real future input frontier");
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true);
    match.until(360);
    for (unsigned peer = 0; peer < 2; ++peer) {
        expect(match.adapters[peer]->state_hash() == control.adapters[0]->state_hash(), "adaptive reconnect equals uninterrupted state");
        expect(match.adapters[peer]->simulation().record() == control.adapters[0]->simulation().record(),
            "adaptive reconnect preserves commands during a delay transition");
        expect(match.sessions[peer]->input_delay() == (decrease ? 4U : 8U), "reconnect restores active timing policy");
        expect(match.sessions[peer]->input_horizon() == net::sealed_input_horizon(decrease ? 4U : 8U),
            "reconnect timing transition envelope drains normally");
    }
    expect(match.sessions[1]->reconnect_status().phase == net::ReconnectPhase::playing,
        "returning seat resumes after replaying timing controls");
    return match.adapters[0]->state_hash();
}

std::string retained_timing_expiry(unsigned workers) {
    unsigned retained{};
    Match match(workers, 3, false, 15, 240, [&](auto& options) {
        options.input_delay = 4; options.non_lan = true;
        options.adaptive_timing = net::AdaptiveTiming{0, true, [](auto) { return std::optional<std::uint64_t>{330}; }};
        options.retain_timing_event = [&](auto tick, auto) {
            expect(tick == 165, "reconnect retains the original timing activation");
            ++retained; return eawr::core::Result<void>::success();
        };
    }, true);
    match.until(140);
    expect(retained == 3, "every reconnect peer retains the future timing event before loss");
    match.hold(2);
    const auto barrier = match.sessions[0]->reconnect_status();
    expect(barrier.barrier_tick < 165 && match.sessions[0]->input_delay() == 4,
        "reconnect hold preserves the future timing event without applying it early");
    for (unsigned round = 0; round < 400; ++round) match.pump();
    for (const auto peer : {0U, 1U}) {
        expect(match.adapters[peer]->completed_tick() == 240 && match.sessions[peer]->input_delay() == 8
            && match.sessions[peer]->reconnect_status().phase == net::ReconnectPhase::playing,
            "expired reconnect seat departs and retained timing applies through match end");
        expect(match.adapters[peer]->simulation().departure_controls().size() == 1,
            "reconnect expiry applies exactly one departure");
    }
    expect(retained == 3 && match.adapters[0]->state_hash() == match.adapters[1]->state_hash()
        && match.adapters[0]->simulation().record() == match.adapters[1]->simulation().record(),
        "reconnect timing expiry retains one control and equal survivor replay/hash");
    return match.adapters[0]->state_hash();
}

void expiry(bool monitored = false, std::uint32_t peers = 3) {
    Match match(1, peers, monitored, 15);
    const auto target = peers - 1;
    match.until(8); match.hold(target);
    std::vector<bool> quits(target);
    for (unsigned round = 0; round < 100; ++round) {
        match.network.round = match.now;
        for (net::PeerIndex peer = 0; peer < target; ++peer) {
            const auto result = require(match.sessions[peer]->pump(match.now));
            quits[peer] = quits[peer] || !result.departures.empty();
        }
        ++match.now;
        if (std::all_of(quits.begin(), quits.end(), [](bool value) { return value; })) break;
    }
    expect(std::all_of(quits.begin(), quits.end(), [](bool value) { return value; }), "grace expiry executes agreed ordinary departure");
    for (net::PeerIndex peer = 1; peer < target; ++peer)
        expect(match.adapters[0]->state_hash() == match.adapters[peer]->state_hash(), "expiry leaves survivors equal");
}

void during_vote() {
    Match match(1, 3, false, 200, 50, {}, true);
    match.until(8);
    match.network.open[2] = false;
    match.pump(2);
    expect(match.sessions[0]->reconnect_status().phase == net::ReconnectPhase::agreeing, "three peers enter a departure vote");
    match.network.inbox[2].clear(); match.network.open[2] = true; match.replace(2, true);
    match.until(50);
    expect(match.adapters[0]->state_hash() == match.adapters[2]->state_hash(), "return during vote waits for recovered frontier");
}

void control_log() {
    Match match(1);
    for (auto& session : match.sessions) {
        require(session->retain_reconnect_control(2, Bytes{56, 2}));
        require(session->retain_reconnect_control(30, Bytes{56, 30}));
    }
    match.until(8); match.hold(1);
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true);
    match.until(50);
    expect(match.controls[1] == std::vector<std::pair<std::uint64_t, Bytes>>{{2, {56, 2}}, {30, {56, 30}}},
        "executed and future timing controls replay once at their original completed tick");
}

void rejection(bool corrupt_log, bool wrong_token, bool stale_epoch) {
    Match match(1);
    match.until(8); match.hold(1);
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true, stale_epoch ? 9 : 1);
    match.network.fault = [=](auto, auto, auto bytes) {
        eawr::testing::FakeDelivery delivery;
        if (corrupt_log && bytes.front() == net::rejoin_chunk_kind) delivery.drop = true;
        return delivery;
    };
    if (wrong_token) {
        match.peer_keys[1][0].fill(99);
        match.replace(1, true);
    }
    bool failed{};
    for (unsigned round = 0; round < 300 && !failed; ++round) {
        match.network.round = match.now;
        for (auto& session : match.sessions) {
            const auto result = session->pump(match.now);
            if (!result) { failed = true; break; }
        }
        ++match.now;
    }
    expect(failed, "invalid identity/epoch or incomplete log cannot activate held seat");
    expect(match.adapters[0]->completed_tick() < 50, "rejected rejoin leaves live match paused");
}

void late_expired_proof() {
    Match match(1, 3, false, 15);
    match.until(8); match.hold(2);
    match.network.inbox[2].clear(); match.network.open[2] = true; match.replace(2, true);
    match.network.fault = [](auto from, auto, auto bytes) {
        eawr::testing::FakeDelivery delivery;
        if (from == 2 && bytes.front() == net::rejoin_proof_kind) delivery.ordered_delay = 25;
        return delivery;
    };
    std::optional<std::uint64_t> departure_at, proof_due;
    bool late{}, returner_timed_out{};
    for (unsigned round = 0; round < 100; ++round) {
        match.network.round = match.now;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            const bool delivering_proof = peer == 0 && proof_due && match.now >= *proof_due
                && std::any_of(match.network.inbox[0].begin(), match.network.inbox[0].end(), [&](const auto& entry) {
                    return entry.due <= match.now && entry.received.from == 2 && entry.received.frame.front() == net::rejoin_proof_kind;
                });
            const auto result = require(match.sessions[peer]->pump(match.now));
            if (!result.departures.empty() && !departure_at) departure_at = match.now;
            if (delivering_proof && departure_at && match.now > *departure_at) {
                expect(match.sessions[0]->reconnect_notice() && match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::stale,
                    "late proof is observed and classified as a completed epoch");
                late = true;
            }
        }
        // The rejected returner can time out; that cannot stop the survivors.
        const auto returning = match.sessions[2]->pump(match.now);
        if (!returning) {
            expect(returning.error().code == net::reconnect_errors::timeout, "expired returner has a progress timeout");
            returner_timed_out = true;
        }
        for (const auto& entry : match.network.inbox[0])
            if (entry.received.from == 2 && entry.received.frame.front() == net::rejoin_proof_kind) proof_due = entry.due;
        ++match.now;
    }
    expect(departure_at && proof_due && late && returner_timed_out, "late proof after expiry is delivered without stopping departure");
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash(), "expiry/proof race preserves survivor equality");
}

Bytes hello(const Match& match, net::PeerIndex peer, std::uint64_t epoch) {
    Bytes frame{net::rejoin_hello_kind};
    for (unsigned byte = 0; byte < 8; ++byte) frame.push_back(static_cast<std::uint8_t>(epoch >> (byte * 8U)));
    frame.push_back(net::rejoin_request);
    for (unsigned byte = 0; byte < 4; ++byte) frame.push_back(static_cast<std::uint8_t>(match.seats[peer].seat.value >> (byte * 8U)));
    const auto& reconnect = *match.options.reconnect;
    frame.insert(frame.end(), reconnect.match_identity.begin(), reconnect.match_identity.end());
    return frame;
}

void authentication_vector() {
    std::array<std::uint8_t, 32> token{}, identity{}, nonce{};
    for (unsigned index = 0; index < 32; ++index) {
        token[index] = static_cast<std::uint8_t>(index);
        identity[index] = static_cast<std::uint8_t>(index + 32);
        nonce[index] = static_cast<std::uint8_t>(index + 64);
    }
    const auto tag = net::rejoin_authenticator(token, identity, 0x12345678, 0x0102030405060708, nonce);
    std::ostringstream hex;
    for (auto byte : tag) hex << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
    // Independently calculated with Python's standard-library hmac/sha256.
    expect(hex.str() == "1341da17efae2643567669c015871d20deb8231333d1dbca466a0df29455a042",
        "canonical HMAC-SHA256 matches independent vector");
    expect(tag != net::rejoin_authenticator(token, identity, 0x12345678, 0x0102030405060708, nonce, false),
        "transfer and survivor-link authentication domains are distinct");
    auto other = nonce; other[0] ^= 1;
    expect(tag != net::rejoin_authenticator(token, identity, 0x12345678, 0x0102030405060708, other), "nonce is authenticated");
    other = identity; other[0] ^= 1;
    expect(tag != net::rejoin_authenticator(token, other, 0x12345678, 0x0102030405060708, nonce), "match is authenticated");
    expect(tag != net::rejoin_authenticator(token, identity, 0x12345679, 0x0102030405060708, nonce), "seat is authenticated");
    expect(tag != net::rejoin_authenticator(token, identity, 0x12345678, 0x0102030405060709, nonce), "epoch is authenticated");
}

void authentication_rejections() {
    Match match(1);
    match.until(8); match.hold(1);
    Bytes response, challenge;
    unsigned offers{};
    match.network.before_send = [&](auto from, auto to, auto frame) {
        if (frame.front() == net::rejoin_offer_kind) ++offers;
        if (frame.front() != net::rejoin_hello_kind) return;
        if (frame[9] == net::rejoin_response) response.assign(frame.begin(), frame.end());
        if (frame[9] == net::rejoin_challenge) challenge.assign(frame.begin(), frame.end());
        const auto& secret = match.peer_keys[from][to];
        expect(std::search(frame.begin(), frame.end(), secret.begin(), secret.end()) == frame.end(),
            "HELLO never contains sender seat secret");
    };
    match.network.fault = [](auto, auto, auto frame) {
        eawr::testing::FakeDelivery action;
        if (frame.front() == net::rejoin_hello_kind && frame[9] == net::rejoin_response) action.drop = true;
        return action;
    };
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true);
    for (unsigned round = 0; round < 8; ++round) match.pump();
    expect(response.size() == 110 && challenge.size() == 78 && offers == 0,
        "survivor sends nonce but no state before HMAC authentication");
    if (response.size() != 110) return;
    const auto inject = [&](Bytes frame) {
        match.network.inbox[0].push_back({match.now, {1, std::move(frame)}});
        match.network.round = match.now;
        require(match.sessions[0]->pump(match.now++));
    };
    auto forged = response; forged.back() ^= 1; inject(forged);
    expect(match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::auth && offers == 0,
        "wrong HMAC rejected without releasing state or failing survivor");
    forged = response; forged[46] ^= 1; inject(forged);
    expect(match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::nonce && offers == 0,
        "response nonce must match issued challenge");
    forged = response; forged[1] = 9; inject(forged);
    expect(match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::stale && offers == 0,
        "response from stale epoch cannot authenticate");
    Bytes progress{net::rejoin_barrier_kind, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    inject(progress);
    expect(match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::auth && offers == 0,
        "progress cannot announce authentication on behalf of survivor");
    Bytes proof{net::rejoin_proof_kind, 1, 0, 0, 0, 0, 0, 0, 0};
    const auto hash = match.adapters[0]->state_hash(); proof.insert(proof.end(), hash.begin(), hash.end()); inject(proof);
    expect(match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::auth && offers == 0,
        "public state hash cannot bypass seat authentication");
    inject(response); inject(response);
    expect(offers == 1 && match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::nonce,
        "nonce consumed once and duplicate response does not resend catch-up");
    match.network.fault = {};
    match.until(50);
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash(), "valid HMAC after rejected forgeries rejoins");
}

void authentication_retry() {
    Match match(1);
    match.until(8); match.hold(1);
    unsigned challenges{}, responses{}, offers{};
    Bytes nonce;
    match.network.before_send = [&](auto, auto, auto frame) {
        if (frame.front() == net::rejoin_offer_kind) ++offers;
        if (frame.front() == net::rejoin_hello_kind && frame[9] == net::rejoin_challenge) {
            const Bytes current(frame.begin() + 46, frame.end());
            if (nonce.empty()) nonce = current;
            expect(nonce == current, "request retry reuses outstanding challenge idempotently");
        }
    };
    match.network.fault = [&](auto, auto, auto frame) {
        eawr::testing::FakeDelivery action;
        if (frame.front() == net::rejoin_hello_kind) {
            if (frame[9] == net::rejoin_challenge) action.drop = ++challenges == 1;
            if (frame[9] == net::rejoin_response) action.drop = ++responses == 1;
        }
        return action;
    };
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true);
    match.until(50);
    expect(challenges >= 3 && responses >= 2 && offers == 1, "lost challenge and response retry without duplicate transfer");
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash(), "authentication retries preserve equality");
}

void authentication_announcement_race() {
    Match match(1, 3);
    match.until(8); match.hold(2);
    std::optional<std::uint64_t> announcement_due, proof_due;
    match.network.before_send = [&](auto from, auto to, auto frame) {
        if (from == 0 && to == 1 && frame.front() == net::rejoin_barrier_kind && frame.size() == 22 && frame[9] == 0)
            announcement_due = match.now + 21;
        if (from == 2 && to == 1 && frame.front() == net::rejoin_proof_kind) proof_due = match.now + 1;
    };
    match.network.fault = [](auto from, auto to, auto frame) {
        eawr::testing::FakeDelivery action;
        if (from == 0 && to == 1 && frame.front() == net::rejoin_barrier_kind && frame.size() == 22 && frame[9] == 0)
            action.ordered_delay = 20;
        return action;
    };
    match.network.inbox[2].clear(); match.network.open[2] = true; match.replace(2, true);
    match.until(50);
    expect(announcement_due && proof_due && *proof_due < *announcement_due,
        "returning state proof arrives before survivor authentication announcement");
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash()
        && match.adapters[0]->state_hash() == match.adapters[2]->state_hash(),
        "cross-stream authentication announcement race preserves equality");
}

void unauthenticated_survivor_stream() {
    Match match(1, 3);
    match.until(8); match.hold(2);
    unsigned link_responses{}, survivor_proofs{};
    bool announced{};
    match.network.before_send = [&](auto from, auto to, auto frame) {
        if (from == 0 && to == 1 && frame.front() == net::rejoin_barrier_kind && frame.size() == 22 && frame[9] == 0)
            announced = true;
        if (from == 2 && to == 1 && frame.front() == net::rejoin_hello_kind && frame[9] == net::rejoin_link_response)
            ++link_responses;
        if (from == 1 && frame.front() == net::rejoin_proof_kind) ++survivor_proofs;
    };
    match.network.fault = [](auto from, auto to, auto frame) {
        eawr::testing::FakeDelivery action;
        // The nonce request reaches survivor 1, but its stream never proves it.
        if (from == 2 && to == 1
            && !(frame.front() == net::rejoin_hello_kind && frame[9] == net::rejoin_link_request)) action.drop = true;
        return action;
    };
    match.network.inbox[2].clear(); match.network.open[2] = true; match.replace(2, true);
    for (unsigned round = 0; round < 20 && link_responses == 0; ++round) match.pump();
    expect(announced && link_responses != 0, "issuer authenticates while second survivor stream remains unauthenticated");
    const auto inject = [&](Bytes frame) {
        match.network.inbox[1].push_back({match.now, {2, std::move(frame)}});
        match.network.round = match.now;
        require(match.sessions[1]->pump(match.now++));
        expect(match.sessions[1]->reconnect_notice()->code == net::reconnect_errors::auth,
            "non-issuer rejects held-seat frame without its own HMAC");
    };
    Bytes proof{net::rejoin_proof_kind, 1, 0, 0, 0, 0, 0, 0, 0};
    const auto hash = match.adapters[1]->state_hash(); proof.insert(proof.end(), hash.begin(), hash.end());
    inject(proof); proof.back() ^= 1; inject(proof);
    proof[1] = 9; inject(proof);
    inject(Bytes{net::rejoin_proof_kind, 1});
    inject(Bytes{net::rejoin_ready_kind, 1, 0, 0, 0, 0, 0, 0, 0});
    inject(Bytes{net::rejoin_resume_kind, 1, 0, 0, 0, 0, 0, 0, 0});
    inject(Bytes{net::rejoin_barrier_kind, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0});
    for (unsigned round = 0; round < 10; ++round) match.pump();
    expect(survivor_proofs == 0 && match.sessions[1]->reconnect_status().phase != net::ReconnectPhase::playing,
        "injected proof READY RESUME and progress cannot activate unauthenticated survivor stream");
    expect(!match.sessions[1]->failure(), "wrong-hash unauthenticated proof cannot stop survivor");
}

void unavailable_authenticated_departure() {
    Match match(1, 2, false, 200, 200, [](auto& options) {
        options.reconnect->max_log_bytes = options.reconnect->setup.size() + 1024;
    });
    match.until(100);
    expect(!match.sessions[0]->reconnect_available(), "fixture exhausts reconnect history");
    Bytes challenge;
    match.network.before_send = [&](auto from, auto to, auto frame) {
        if (from == 0 && to == 1 && frame.front() == net::rejoin_hello_kind && frame[9] == net::rejoin_challenge)
            challenge.assign(frame.begin(), frame.end());
    };
    match.network.inbox[0].push_back({match.now, {1, hello(match, 1, 1)}});
    match.network.round = match.now; require(match.sessions[0]->pump(match.now++));
    expect(challenge.size() == 78 && match.sessions[0]->reconnect_status().phase == net::ReconnectPhase::playing,
        "unavailable reconnect still challenges without unauthenticated departure");
    if (challenge.size() != 78) return;
    net::RejoinNonce nonce{}; std::copy(challenge.begin() + 46, challenge.end(), nonce.begin());
    auto response = hello(match, 1, 1); response[9] = net::rejoin_response;
    response.insert(response.end(), nonce.begin(), nonce.end());
    const auto& options = *match.options.reconnect;
    const auto tag = net::rejoin_authenticator(options.seat_tokens[1], options.match_identity, match.seats[1].seat.value, 1, nonce);
    response.insert(response.end(), tag.begin(), tag.end());
    match.network.inbox[0].push_back({match.now, {1, response}});
    bool lost{};
    for (unsigned round = 0; round < 5 && !lost; ++round) {
        match.network.round = match.now;
        lost = connection_lost(match, 0, match.sessions[0]->pump(match.now++));
    }
    expect(lost, "unavailable authenticated replacement ends neutrally without completeness timeout");
}

void authentication_domain_isolation(bool poison_transfer) {
    const auto peers = poison_transfer ? 2U : 3U;
    const net::PeerIndex target = peers - 1;
    const net::PeerIndex survivor = poison_transfer ? 0U : 1U;
    Match match(1, peers);
    match.until(8); match.hold(target);
    match.network.open[target] = true;
    auto request = hello(match, target, 1);
    request[9] = poison_transfer ? net::rejoin_link_request : net::rejoin_request;
    match.network.inbox[survivor].push_back({match.now, {target, request}});
    match.network.round = match.now; require(match.sessions[survivor]->pump(match.now++));
    unsigned offers{};
    match.network.before_send = [&](auto, auto, auto frame) {
        if (frame.front() == net::rejoin_offer_kind) ++offers;
    };
    match.network.inbox[target].clear(); match.replace(target, true);
    match.until(50);
    expect(offers == 1, "unauthenticated request in another domain cannot lock the legitimate handshake");
    for (net::PeerIndex peer = 1; peer < peers; ++peer)
        expect(match.adapters[0]->state_hash() == match.adapters[peer]->state_hash(), "domain isolation preserves valid rejoin equality");
}

void survivor_link_retry(bool lose_response, bool lose_proof = false) {
    Match match(1, 3);
    match.until(8); match.hold(2);
    unsigned responses{}, proofs{}, offers{};
    match.network.before_send = [&](auto, auto, auto frame) {
        if (frame.front() == net::rejoin_offer_kind) ++offers;
    };
    match.network.fault = [&](auto from, auto to, auto frame) {
        eawr::testing::FakeDelivery action;
        if (from == 2 && to == 1 && frame.front() == net::rejoin_hello_kind && frame[9] == net::rejoin_link_response)
            action.drop = ++responses == 1 && lose_response;
        if (from == 1 && to == 2 && frame.front() == net::rejoin_proof_kind)
            action.drop = ++proofs == 1 && lose_proof;
        return action;
    };
    match.network.inbox[2].clear(); match.network.open[2] = true; match.replace(2, true);
    match.until(50);
    expect(match.sessions[2]->reconnect_status().phase == net::ReconnectPhase::playing && offers == 1,
        "survivor-link handshake completes after response/proof retry without duplicate transfer");
    expect(responses >= (lose_response || lose_proof ? 2U : 1U), "link requests continue until survivor proof acknowledges authentication");
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash()
        && match.adapters[0]->state_hash() == match.adapters[2]->state_hash(), "survivor-link retries preserve equality");
}

void unauthenticated_request() {
    Match match(1);
    match.until(8);
    match.network.inbox[0].push_back({match.now, {1, hello(match, 1, 1)}});
    match.pump();
    expect(match.sessions[0]->reconnect_status().phase == net::ReconnectPhase::playing,
        "unauthenticated request cannot freeze a live match");
    match.until(50);
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash(), "challenge alone cannot mutate gameplay");
}

void returning_host() {
    Match match(2);
    match.until(8); match.hold(0);
    match.network.inbox[0].clear(); match.network.open[0] = true; match.replace(0, true);
    match.until(50);
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash(), "original host seat authenticates through survivor");
}

void old_nonce_new_incident() {
    Match match(1, 2, false, 200, 100);
    Bytes old_response, new_response;
    match.network.before_send = [&](auto, auto, auto frame) {
        if (frame.front() == net::rejoin_hello_kind && frame[9] == net::rejoin_response) {
            if (frame[1] == 1) old_response.assign(frame.begin(), frame.end());
            else new_response.assign(frame.begin(), frame.end());
        }
    };
    match.until(8); match.hold(1);
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true);
    match.until(30); match.hold(1);
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true, 2);
    match.network.fault = [](auto, auto, auto frame) {
        eawr::testing::FakeDelivery action;
        if (frame.front() == net::rejoin_hello_kind && frame[9] == net::rejoin_response) action.drop = true;
        return action;
    };
    for (unsigned round = 0; round < 8; ++round) match.pump();
    expect(old_response.size() == 110 && new_response.size() == 110
        && !std::equal(old_response.begin() + 46, old_response.begin() + 78, new_response.begin() + 46),
        "new incident receives fresh OS nonce");
    if (old_response.size() != 110 || new_response.size() != 110) return;
    old_response[1] = 2;
    match.network.inbox[0].push_back({match.now, {1, old_response}});
    match.network.round = match.now; require(match.sessions[0]->pump(match.now++));
    expect(match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::nonce,
        "old response with rewritten epoch cannot reuse nonce");
    match.network.fault = {}; match.network.inbox[0].push_back({match.now, {1, new_response}});
    match.until(100);
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash(), "second authenticated incident preserves equality");
}

void early_hello_and_retry() {
    Match match(1);
    match.until(8);
    match.network.inbox[1].clear(); match.replace(1, true);
    unsigned hellos{};
    match.network.fault = [&](auto from, auto, auto frame) {
        eawr::testing::FakeDelivery action;
        if (from == 1 && frame.front() == net::rejoin_hello_kind) action.drop = ++hellos == 1;
        return action;
    };
    match.until(50);
    expect(hellos >= 2 && hellos <= 8, "rejoin HELLO retries are bounded");
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash(), "HELLO before disconnect observation still rejoins");
}

void delayed_held_batch() {
    Match match(1, 2, false, 500);
    match.network.fault = [](auto from, auto, auto bytes) {
        eawr::testing::FakeDelivery action;
        if (from == 1 && bytes.front() == net::sealed_batch_kind) action.ordered_delay = 150;
        return action;
    };
    bool delivered{};
    for (unsigned round = 0; round < 175; ++round) {
        match.network.round = match.now;
        // Closed-stream recovery still holds; open-link application expiry now ends neutrally.
        if (round == 1) match.network.open[1] = false;
        const bool late = std::any_of(match.network.inbox[0].begin(), match.network.inbox[0].end(), [&](const auto& entry) {
            return entry.due <= match.now && entry.received.from == 1 && entry.received.frame.front() == net::sealed_batch_kind;
        });
        require(match.sessions[0]->pump(match.now));
        if (round == 1) match.network.open[1] = true;
        if (round == 0) require(match.sessions[1]->pump(match.now));
        if (late && round > 100) delivered = true;
        ++match.now;
    }
    expect(delivered && match.sessions[0]->reconnect_status().phase == net::ReconnectPhase::held,
        "a closed-stream hold ignores an old batch delivered after the link reopens");
    expect(!match.sessions[0]->failure(), "late WAN traffic does not fail the survivor");
}

void pending_quit_then_new_incident() {
    Match match(1, 3, false, 200, 50, {}, true);
    match.until(8); match.quit(2); match.pump(); match.pump();
    match.network.open[2] = false;
    match.pump(2); // Vote open; original sealed quit has not executed yet.
    expect(match.sessions[0]->reconnect_status().phase == net::ReconnectPhase::agreeing, "quit/disconnect opens a vote");
    match.network.inbox[0].push_back({match.now, {2, hello(match, 2, 1)}});
    bool quit_seen{};
    for (unsigned round = 0; round < 30; ++round) {
        match.network.round = match.now;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            const auto result = require(match.sessions[peer]->pump(match.now));
            quit_seen = quit_seen || !result.departures.empty();
        }
        ++match.now;
        if (quit_seen) break;
    }
    expect(quit_seen, "sealed quit remains an ordinary departure");
    match.hold(1);
    const auto epoch = match.sessions[0]->reconnect_status().epoch;
    expect(epoch == 2, "ordinary departure consumes its parked handshake epoch");
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true, epoch);
    // Seat 2 has permanently left; only the two remaining sessions are pumped.
    for (unsigned round = 0; round < 200; ++round) {
        match.network.round = match.now;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) require(match.sessions[peer]->pump(match.now));
        ++match.now;
        if (match.adapters[0]->completed_tick() == 50 && match.adapters[1]->completed_tick() == 50) break;
    }
    expect(match.adapters[0]->completed_tick() == 50 && match.adapters[1]->completed_tick() == 50,
        "parked quit HELLO cannot poison a later rejoin");
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash(), "later incident preserves world equality");
}

void replayed_handshake() {
    Match match(1);
    match.until(8); match.hold(1);
    std::vector<net::Received> captured;
    unsigned duplicate_hellos{};
    match.network.before_send = [&](auto from, auto to, auto frame) {
        if (to == 0 && frame.front() == net::rejoin_hello_kind) captured.push_back({from, Bytes(frame.begin(), frame.end())});
        if (to == 1 && frame.front() >= net::rejoin_offer_kind && frame.front() <= net::rejoin_barrier_kind)
            captured.push_back({from, Bytes(frame.begin(), frame.end())});
    };
    match.network.fault = [&](auto, auto, auto frame) {
        eawr::testing::FakeDelivery action;
        if (frame.front() == net::rejoin_hello_kind) { action.duplicate = true; action.duplicate_delay = 3; ++duplicate_hellos; }
        return action;
    };
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true);
    match.until(50);
    expect(duplicate_hellos != 0, "HELLO duplicate after offer is exercised");
    for (const auto& frame : captured) match.network.inbox[1U - frame.from].push_back({match.now, frame});
    match.pump();
    expect(!match.sessions[0]->failure() && !match.sessions[1]->failure(), "old HELLO/OFFER/CHUNK/BARRIER cannot end a resumed match");
    expect(match.sessions[0]->reconnect_notice() && match.sessions[1]->reconnect_notice(), "stale handshake emits a diagnostic");
}

void budget_exhaustion() {
    Match match(1, 2, false, 200, 200, [](auto& options) { options.reconnect->max_log_bytes = options.reconnect->setup.size() + 1024; });
    match.until(100);
    for (const auto& session : match.sessions) {
        expect(!session->reconnect_available() && !session->failure(), "budget exhaustion disables rejoin without failing play");
        expect(session->reconnect_notice() && session->reconnect_notice()->code == net::reconnect_errors::unavailable,
            "budget exhaustion emits a persistent typed notice");
    }
    match.network.open[1] = false;
    bool lost{};
    for (unsigned round = 0; round < 30 && !lost; ++round) {
        match.network.round = match.now;
        lost = connection_lost(match, 0, match.sessions[0]->pump(match.now++));
    }
    expect(lost, "drop after budget exhaustion ends without an unconfirmed lone-survivor win");
}

void slow_transfer(bool cap) {
    Match match(1, 2, false, 15, 50, [cap](auto& options) {
        options.reconnect->progress_timeout_ms = cap ? 20 : 50;
        options.reconnect->hard_cap_ms = cap ? 35 : 150;
    });
    for (auto& session : match.sessions) {
        require(session->retain_reconnect_control(1, Bytes(300'000, 56)));
    }
    match.until(8); match.hold(1);
    unsigned chunks{};
    match.network.fault = [&](auto from, auto, auto bytes) {
        eawr::testing::FakeDelivery action;
        if (from == 0 && bytes.front() == net::rejoin_chunk_kind) action.ordered_delay = cap ? 10 * ++chunks : 20;
        return action;
    };
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true);
    if (!cap) {
        match.until(50);
        expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash(), "healthy transfer longer than grace completes with progress");
    } else {
        bool lost{}, timed_out{};
        for (unsigned round = 0; round < 150; ++round) {
            match.network.round = match.now;
            if (!lost) lost = connection_lost(match, 0, match.sessions[0]->pump(match.now));
            const auto target = match.sessions[1]->pump(match.now);
            if (!target) { expect(target.error().code == net::reconnect_errors::timeout, "hard-cap returner times out"); timed_out = true; }
            ++match.now;
        }
        expect(chunks >= 4 && lost && timed_out, "hard cap ends a transfer neutrally despite earlier accepted progress");
    }
}

void grace_boundary() {
    Match match(1, 2, false, 15);
    match.hold(1); // Empty frontier: hold begins at caller time zero.
    match.network.round = 14;
    require(match.sessions[0]->pump(14));
    expect(match.sessions[0]->reconnect_status().phase == net::ReconnectPhase::held, "grace minus one still holds");
    match.network.round = 15;
    require(match.sessions[0]->pump(15));
    expect(connection_lost(match, 0, match.sessions[0]->pump(15)),
        "grace expires exactly at its agreed boundary without inventing a departure");
}

void ready_crosses_hard_cap() {
    Match match(1, 2, false, 15, 50, [](auto& options) {
        options.reconnect->progress_timeout_ms = 100;
        options.reconnect->hard_cap_ms = 120;
    });
    match.until(8);
    const auto cap_at = match.now + 120; // The two-seat hold opens on this next pump.
    match.hold(1);
    std::optional<std::uint64_t> committed_at, ready_due;
    match.network.before_send = [&](auto from, auto, auto bytes) {
        if (from == 0 && bytes.front() == net::rejoin_ready_kind) committed_at = match.network.round;
    };
    match.network.fault = [](auto from, auto, auto bytes) {
        eawr::testing::FakeDelivery action;
        if (from == 0 && bytes.front() == net::rejoin_chunk_kind) action.ordered_delay = 80;
        if (from == 1 && bytes.front() == net::rejoin_ready_kind) action.ordered_delay = 45;
        return action;
    };
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true);
    for (unsigned round = 0; round < 300; ++round) {
        match.pump();
        for (const auto& entry : match.network.inbox[0])
            if (entry.received.from == 1 && entry.received.frame.front() == net::rejoin_ready_kind) ready_due = entry.due;
        if (match.adapters[0]->completed_tick() == 50 && match.adapters[1]->completed_tick() == 50) break;
    }
    expect(committed_at && ready_due && *committed_at < cap_at && *ready_due > cap_at,
        "READY is sent before the hard cap and delivered after it");
    expect(match.adapters[0]->completed_tick() == 50 && match.adapters[1]->completed_tick() == 50
        && match.adapters[0]->state_hash() == match.adapters[1]->state_hash(),
        "committed rejoin crosses the admission cap and resumes without failing the match");
}

void recovery_timeout_bounds() {
    Match match(1, 2, true);
    const std::array<std::uint64_t net::ReconnectOptions::*, 4> intervals{
        &net::ReconnectOptions::grace_ms, &net::ReconnectOptions::progress_timeout_ms,
        &net::ReconnectOptions::hard_cap_ms, &net::ReconnectOptions::hello_retry_ms};
    for (const auto interval : intervals) {
        auto options = match.options;
        options.reconnect->hard_cap_ms = net::max_recovery_timeout_ms;
        options.reconnect.value().*interval = net::max_recovery_timeout_ms;
        expect(static_cast<bool>(net::LockstepSession::create(*match.adapters[0], *match.transports[0],
            match.seats, options, match.monitors[0].get())), "maximum reconnect timeout accepted");
        options.reconnect.value().*interval = net::max_recovery_timeout_ms + 1;
        const auto refused = net::LockstepSession::create(*match.adapters[0], *match.transports[0],
            match.seats, options, match.monitors[0].get());
        expect(!refused && refused.error().code == net::reconnect_errors::options,
            "every excessive reconnect timeout is refused at session creation");
    }
    auto options = match.options;
    options.resync.emplace(); options.resync->timeout_ms = net::max_recovery_timeout_ms;
    options.resync->rebuild = [&match](auto) {
        std::unique_ptr<net::SessionAdapter> fresh = std::make_unique<net::TacticalSessionAdapter>(
            require(tactical::TacticalSession::create(setup(), content())), match.executor, match.bindings);
        return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(fresh));
    };
    expect(static_cast<bool>(net::LockstepSession::create(*match.adapters[0], *match.transports[0],
        match.seats, options, match.monitors[0].get())), "maximum resync timeout accepted");
    options.resync->timeout_ms = net::max_recovery_timeout_ms + 1;
    const auto refused = net::LockstepSession::create(*match.adapters[0], *match.transports[0],
        match.seats, options, match.monitors[0].get());
    expect(!refused && refused.error().code == net::resync_errors::options, "excessive resync timeout refused");
}
void wrong_delay() {
    Match match(1);
    match.until(8); match.hold(1);
    match.network.inbox[1].clear(); match.network.open[1] = true;
    match.options.input_delay = 4; match.replace(1, true);
    bool mismatch{};
    for (unsigned round = 0; round < 40; ++round) {
        match.network.round = match.now;
        require(match.sessions[0]->pump(match.now));
        const auto result = match.sessions[1]->pump(match.now++);
        if (!result) { mismatch = result.error().code == net::reconnect_errors::log; break; }
    }
    expect(mismatch, "replayed delay must match the locally agreed policy");
}

std::string overlapping_drop(unsigned workers, bool remote_proposal) {
    Match match(workers, 4, false, 200, 50, {}, true);
    match.until(8); match.hold(0);
    const auto hash = match.adapters[2]->state_hash();
    const auto barrier = match.sessions[2]->reconnect_status();
    if (remote_proposal) {
        const std::array<std::uint8_t, 14> proposal{net::departure_proposal_kind, 1, 0, 0, 0, 1};
        match.network.deliver(3, 2, proposal);
        match.network.deliver(2, 3, proposal);
        ++match.now;
    } else match.network.open[1] = false;
    match.network.round = match.now;
    for (const auto peer : {2U, 3U}) {
        const auto result = match.sessions[peer]->pump(match.now);
        expect(!result && result.error().code == net::departure_errors::reconnect_hold,
            "held-seat overlap has a distinct departure refusal on every survivor");
        const auto repeated = match.sessions[peer]->pump(match.now + 1000);
        expect(!repeated && repeated.error().code == net::departure_errors::reconnect_hold,
            "held-seat overlap refusal is latched before expiry service");
        const auto status = match.sessions[peer]->reconnect_status();
        expect(status.epoch == barrier.epoch && status.peer == 0 && status.barrier_tick == barrier.barrier_tick
            && match.adapters[peer]->state_hash() == hash && match.adapters[peer]->completed_tick() == barrier.barrier_tick,
            "overlap preserves the same held epoch and simulation hash");
        expect(!match.sessions[peer]->submit([](auto) { return eawr::core::Result<Bytes>::success({1}); }),
            "refused held-seat overlap admits no new commands");
    }
    return hash;
}

void returning_survivor_loss(bool remote_proposal) {
    Match match(2, 3, false, 200);
    match.until(8); match.hold(1);
    const auto barrier = match.sessions[0]->reconnect_status();
    match.network.inbox[1].clear(); match.network.open[1] = true;
    match.network.fault = [](auto from, auto to, auto frame) {
        eawr::testing::FakeDelivery delivery;
        delivery.drop = from == 2 && to == 1 && frame.front() == net::rejoin_proof_kind;
        return delivery;
    };
    match.replace(1, true, barrier.epoch);
    bool restored{};
    for (unsigned round = 0; round < 100 && !restored; ++round) {
        match.pump();
        restored = match.adapters[1]->completed_tick() == barrier.barrier_tick;
    }
    expect(restored && match.sessions[1]->reconnect_status().phase != net::ReconnectPhase::playing,
        "returning seat is restored but waiting on its third-party survivor proof");
    const auto hash = match.adapters[1]->state_hash();
    if (remote_proposal) {
        const std::array<std::uint8_t, 14> proposal{net::departure_proposal_kind, 2, 0, 0, 0, 1};
        match.network.deliver(0, 1, proposal);
    } else match.network.open[2] = false;
    match.network.round = ++match.now;
    if (!remote_proposal) {
        // Check the survivor before advancing the returning seat's deadline;
        // queued returning proofs must not turn this into an expiry test.
        const auto survivor = match.sessions[0]->pump(match.now);
        expect(!survivor && survivor.error().code == net::departure_errors::reconnect_hold,
            "existing survivor still fails fast on the overlapping third-seat loss"
            + (survivor ? std::string{} : ": " + survivor.error().code + " " + survivor.error().message));
    }
    auto result = match.sessions[1]->pump(match.now);
    if (!remote_proposal && result) {
        // A returning stream can still be establishing its independent survivor
        // proof. Preserve that bounded policy; survivors themselves fail fast.
        match.network.round = match.now += 201;
        result = match.sessions[1]->pump(match.now);
    }
    expect(!result && result.error().code == (remote_proposal ? net::departure_errors::reconnect_hold : net::reconnect_errors::timeout),
        "third-survivor loss or departure cannot publish the returning world");
    expect(match.adapters[1]->completed_tick() == barrier.barrier_tick && match.adapters[1]->state_hash() == hash,
        "failed returning overlap preserves the independently restored prefix");
}

void desync_vote_during_hold() {
    Match match(1, 3, true, 200, 50, [](auto& options) {
        options.resync = net::ResyncOptions{};
        options.resync->rebuild = [](auto) {
            return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success({});
        };
        options.resync->replay_control = [](auto&, auto) { return eawr::core::Result<void>::success(); };
    });
    match.until(8); match.hold(2);
    Bytes vote{net::resync_vote_kind};
    const auto append = [&](std::uint64_t value, unsigned width) {
        for (unsigned index = 0; index < width; ++index) vote.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    };
    append(2, 8); append(match.adapters[0]->completed_tick(), 8);
    vote.insert(vote.end(), 128, 'a');
    require(match.transports[1]->send(0, vote));
    match.network.round = ++match.now;
    const auto result = match.sessions[0]->pump(match.now);
    expect(!result && result.error().code == net::resync_errors::overlap,
        "desync recovery during an established reconnect hold fails closed");
    expect(match.sessions[0]->reconnect_status().phase == net::ReconnectPhase::held,
        "refused resync did not preserve the held incident");
    match.network.round = ++match.now;
    const auto other = match.sessions[1]->pump(match.now);
    expect(!other && other.error().code == net::resync_errors::overlap,
        "pre-incident refusal did not immediately abort the other held voter");
}

void corrupted_state_during_hold() {
    unsigned rebuilds{};
    Match match(2, 3, true, 15, 50, [&](auto& options) {
        options.resync = net::ResyncOptions{};
        options.resync->rebuild = [&](auto) {
            ++rebuilds;
            return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success({});
        };
        options.resync->replay_control = [](auto&, auto) { return eawr::core::Result<void>::success(); };
    });
    match.until(8); match.hold(2);
    const auto tick = match.adapters[0]->completed_tick();
    auto record = match.adapters[0]->simulation().record();
    ++record.setup.seed;
    auto corrupted = require(tactical::TacticalSession::from_replay(record, content()));
    while (corrupted.completed_tick() < tick) require(corrupted.step(match.executor));
    // Replace the test-owned adapter in place; the product's const world view is untouched.
    *match.adapters[0] = net::TacticalSessionAdapter(std::move(corrupted), match.executor, match.bindings);
    expect(match.adapters[0]->state_hash() != match.adapters[1]->state_hash(), "held-state injection did not diverge");
    bool failed{};
    for (unsigned round = 0; round < 100 && !failed; ++round) {
        match.network.round = match.now;
        for (net::PeerIndex peer = 0; peer < 2; ++peer) {
            const auto result = match.sessions[peer]->pump(match.now);
            if (!result) { failed = result.error().code == net::reconnect_errors::hash; break; }
        }
        ++match.now;
    }
    expect(failed && rebuilds == 0 && match.adapters[0]->completed_tick() == tick,
        "divergence during reconnect hold must fail the existing hash proof without resync");
}

void reconnect_resync_reconnect() {
    Match* current{};
    unsigned rebuilds{};
    Match match(2, 2, true, 1000, 90, [&](auto& options) {
        options.resync = net::ResyncOptions{};
        options.resync->rebuild = [&](auto bytes) {
            expect(Bytes(bytes.begin(), bytes.end()) == current->options.reconnect->setup,
                "composition factory received a different verified setup");
            ++rebuilds;
            std::unique_ptr<net::SessionAdapter> fresh = std::make_unique<net::TacticalSessionAdapter>(
                require(tactical::TacticalSession::create(setup(current->player_count), content())), current->executor, current->bindings);
            return eawr::core::Result<std::unique_ptr<net::SessionAdapter>>::success(std::move(fresh));
        };
        options.resync->replay_control = [](auto&, auto) { return eawr::core::Result<void>::success(); };
    });
    current = &match;
    for (unsigned peer = 0; peer < 2; ++peer) match.submit(peer);
    match.until(8); match.hold(1);
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true, 1);
    match.until(20);
    expect(match.sessions[1]->reconnect_status().phase == net::ReconnectPhase::playing,
        "resync-enabled returning seat did not activate");
    auto record = match.adapters[1]->simulation().record();
    ++record.setup.seed;
    auto corrupted = require(tactical::TacticalSession::from_replay(record, content()));
    const auto tick = match.adapters[1]->completed_tick();
    while (corrupted.completed_tick() < tick) require(corrupted.step(match.executor));
    *match.adapters[1] = net::TacticalSessionAdapter(std::move(corrupted), match.executor, match.bindings);
    for (unsigned round = 0; round < 500; ++round) {
        match.pump();
        if (match.sessions[0]->active_adapter().completed_tick() >= 35
            && match.sessions[1]->active_adapter().completed_tick() >= 35) break;
    }
    expect(rebuilds == 1 && match.monitors[1]->recovered_report()
        && match.sessions[0]->active_adapter().state_hash() == match.sessions[1]->active_adapter().state_hash(),
        "returning seat lost the agreed recovery policy");
    expect(&match.sessions[1]->active_adapter() != match.adapters[1].get(), "resync publishes its replacement adapter");
    const auto published_tick = match.sessions[1]->active_adapter().completed_tick();
    const auto retired_tick = match.adapters[1]->completed_tick();
    for (unsigned round = 0; round < 100 && match.sessions[1]->active_adapter().completed_tick() == published_tick; ++round) match.pump();
    expect(match.sessions[1]->active_adapter().completed_tick() > published_tick && match.adapters[1]->completed_tick() == retired_tick,
        "recovered play advances active_adapter while the retired adapter remains unchanged");
    // Rejoin epoch 1, repair epoch 2, then another held-seat incident at epoch 3.
    match.hold(1);
    expect(match.sessions[0]->reconnect_status().epoch == 3, "resync and reconnect did not share epoch progression");
    match.network.inbox[1].clear(); match.network.open[1] = true; match.replace(1, true, 3);
    match.until(90);
    expect(match.adapters[0]->state_hash() == match.adapters[1]->state_hash()
        && match.sessions[1]->reconnect_status().phase == net::ReconnectPhase::playing,
        "reconnect after a proved recovery lost state or membership");
}

void recovery_without_rejoin() {
    Match match(2, 2, false, 200, 50, [](net::SessionOptions& options) {
        options.reconnect->allow_rejoin = false; options.reconnect->seat_tokens.clear();
    });
    match.until(8);
    const auto before = match.adapters[0]->completed_tick();
    require(match.transports[1]->send(0, Bytes{net::rejoin_hello_kind}));
    match.pump();
    expect(match.sessions[0]->reconnect_notice() && match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::unavailable,
        "disabled rejoin ignores HELLO with unavailable notice");
    match.until(12);
    expect(match.adapters[0]->completed_tick() > before && !match.sessions[0]->reconnect_available(), "stray HELLO cannot stop play");
    match.network.open[1] = false;
    bool lost{};
    for (unsigned round = 0; round < 100 && !lost; ++round) {
        match.network.round = match.now;
        lost = connection_lost(match, 0, match.sessions[0]->pump(match.now++));
    }
    expect(lost && match.sessions[0]->reconnect_status().phase != net::ReconnectPhase::held,
        "history-only recovery ends neutrally without a reconnect hold");
}

template<class Function> void run_case(std::string_view name, Function function) {
    try { function(); }
    catch (const std::exception& error) { ++failures; std::cerr << name << ": " << error.what() << '\n'; }
}
void erased_keys_ignore_rejoin() {
    Match match(1); match.until(8); match.hold(1);
    match.sessions[0]->erase_reconnect_credentials();
    match.network.open[1] = true;
    match.network.inbox[0].push_back({match.now, {1, hello(match, 1, 1)}});
    match.network.round = match.now;
    require(match.sessions[0]->pump(match.now));
    expect(match.sessions[0]->reconnect_notice() && match.sessions[0]->reconnect_notice()->code == net::reconnect_errors::unavailable,
        "late rejoin after terminal key erasure produces a notice without indexing released storage");
}
}

void replacement_pair_proof() {
    net::RejoinNonce match{}, key{}; match.fill(14); key.fill(33);
    net::RejoinLinkProof a(match, key, 0, 2, 7, "EAWR-GNS-rejoin-v1"), b(match, key, 2, 0, 7, "EAWR-GNS-rejoin-v1");
    auto ac = require(a.start()), bc = require(b.start());
    expect(!a.proven() && !b.proven(), "no pair is published on the room capability alone");
    auto ar = require(a.receive(bc)), br = require(b.receive(ac));
    expect(ar && br && !a.proven() && !b.proven(), "a challenge alone cannot prove a pair");
    require(a.receive(*br)); require(b.receive(*ar));
    expect(a.proven() && b.proven(), "both pair keys prove the replacement connection");
    net::RejoinLinkProof replay(match, key, 0, 2, 7, "EAWR-GNS-rejoin-v1");
    require(replay.start());
    expect(!replay.receive(*br), "a captured proof cannot answer a fresh OS nonce");
    net::RejoinLinkProof epoch(match, key, 0, 2, 8, "EAWR-GNS-rejoin-v1"); require(epoch.start());
    expect(!epoch.receive(bc), "a stale incident cannot authenticate a replacement");
    net::RejoinLinkProof wrong_pair(match, key, 0, 1, 7, "EAWR-GNS-rejoin-v1"); require(wrong_pair.start());
    expect(!wrong_pair.receive(bc), "a proof cannot move to another pair");
    net::RejoinLinkProof wrong_domain(match, key, 0, 2, 7, "EAWR-TCP-rejoin-v1"); require(wrong_domain.start());
    expect(!wrong_domain.receive(bc), "proofs are transport-domain bound");
    auto bad_key = key; bad_key[0] ^= 1;
    net::RejoinLinkProof attacker(match, bad_key, 2, 0, 7, "EAWR-GNS-rejoin-v1");
    auto challenge = require(attacker.start());
    net::RejoinLinkProof victim(match, key, 0, 2, 7, "EAWR-GNS-rejoin-v1");
    auto fresh = require(victim.start()); require(victim.receive(challenge));
    auto forged = require(attacker.receive(fresh));
    expect(forged && !victim.receive(*forged), "the right room identity with the wrong key cannot send data");
    expect(!victim.receive(Bytes{1}), "application bytes before proof are refused");
}

int main() {
    replacement_pair_proof();
    std::string expiry_reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) run_case("retained timing expiry", [&] {
        const auto hash = retained_timing_expiry(workers);
        if (workers == 1) expiry_reference = hash;
        expect(hash == expiry_reference, "retained timing reconnect expiry agrees on 1/2/4/8 workers");
        std::cout << "retained timing expiry workers=" << workers << " hash=" << hash << '\n';
    });
    for (const auto drop_tick : {140U, 162U, 276U, 292U}) {
        std::string reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) run_case("adaptive reconnect", [&] {
            const auto hash = adaptive_reconnect(workers, drop_tick >= 256, drop_tick);
            if (reference.empty()) reference = hash;
            expect(hash == reference, "adaptive reconnect remains equal on 1/2/4/8 workers");
        });
    }
    run_case("monitored adaptive reconnect", [] { adaptive_reconnect(2, false, 140, true); });
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        run_case("worker equality", [&] {
            const auto hash = reconnect(workers, 8, 2, false);
            if (baseline.empty()) baseline = hash;
            expect(hash == baseline, "reconnect is deterministic on 1/2/4/8 workers");
            std::cout << "reconnect workers=" << workers << " state=" << hash << '\n';
        });
    }
    run_case("history only recovery", recovery_without_rejoin);
    run_case("erased reconnect keys", erased_keys_ignore_rejoin);
    run_case("connecting replacement", [] { connecting_rejoin(false); });
    run_case("replacement authentication deadline", [] { connecting_rejoin(true); });
    run_case("HMAC vector", authentication_vector);
    run_case("authentication rejections", authentication_rejections);
    run_case("authentication retry", authentication_retry);
    run_case("authentication announcement race", authentication_announcement_race);
    run_case("unauthenticated survivor stream", unauthenticated_survivor_stream);
    run_case("unavailable authenticated departure", unavailable_authenticated_departure);
    run_case("link request cannot poison transfer", [] { authentication_domain_isolation(true); });
    run_case("transfer request cannot poison link", [] { authentication_domain_isolation(false); });
    run_case("survivor-link control", [] { survivor_link_retry(false); });
    run_case("lost survivor-link response", [] { survivor_link_retry(true); });
    run_case("lost survivor-link proof", [] { survivor_link_retry(false, true); });
    run_case("unauthenticated request", unauthenticated_request);
    run_case("returning host", returning_host);
    run_case("old nonce new incident", old_nonce_new_incident);
    run_case("tick zero", [] { reconnect(2, 0, 2, false); });
    run_case("three peers", [] { reconnect(2, 23, 3, false); });
    run_case("monitored rejoin", [] { reconnect(2, 8, 2, true); });
    run_case("return during vote", during_vote);
    run_case("expiry", [] { expiry(); });
    run_case("monitored expiry", [] { expiry(true); });
    run_case("control log", control_log);
    run_case("expiry proof race", late_expired_proof);
    run_case("early HELLO and retry", early_hello_and_retry);
    run_case("delayed held batch", delayed_held_batch);
    run_case("parked HELLO after quit", pending_quit_then_new_incident);
    run_case("replayed handshake", replayed_handshake);
    run_case("budget exhaustion", budget_exhaustion);
    run_case("slow transfer", [] { slow_transfer(false); });
    run_case("transfer hard cap", [] { slow_transfer(true); });
    run_case("exact grace boundary", grace_boundary);
    run_case("committed cap boundary", ready_crosses_hard_cap);
    run_case("wrong delay", wrong_delay);
    run_case("recovery timeout bounds", recovery_timeout_bounds);
    run_case("returning third-survivor loss", [] { returning_survivor_loss(false); });
    run_case("returning third-survivor departure", [] { returning_survivor_loss(true); });
    for (const bool remote_proposal : {false, true}) {
        std::string reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) run_case("overlapping loss", [&] {
            const auto hash = overlapping_drop(workers, remote_proposal);
            if (workers == 1) reference = hash;
            expect(hash == reference, "held-seat departure refusal agrees on 1/2/4/8 workers");
            std::cout << "held-seat overlap remote=" << remote_proposal << " workers=" << workers << " hash=" << hash << '\n';
        });
    }
    run_case("desync during reconnect hold", desync_vote_during_hold);
    run_case("corrupt state during reconnect hold", corrupted_state_during_hold);
    run_case("reconnect resync reconnect", reconnect_resync_reconnect);
    run_case("three-survivor expiry", [] { expiry(false, 4); });
    run_case("wrong token", [] { rejection(false, true, false); });
    run_case("stale epoch", [] { rejection(false, false, true); });
    run_case("incomplete log", [] { rejection(true, false, false); });
    return failures == 0 ? 0 : 1;
}
