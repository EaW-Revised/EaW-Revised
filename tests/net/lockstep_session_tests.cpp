#include "eawr/net/tactical_adapter.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "../support/lockstep_loopback.hpp"
#include "fake_transport.hpp"

#include <iostream>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
namespace lockstep = eawr::testing::lockstep;
using eawr::testing::FakeDelivery;
using eawr::testing::FakeNetwork;
using eawr::testing::FakeTransport;
using sim::math::Fixed;

int failures{};
void expect(bool value, const std::string& message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
Fixed units(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

void object_turn_only_command_log() {
    std::vector<std::vector<std::uint8_t>> input_reference, log_reference;
    std::vector<std::string> hash_reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup initial;
        initial.players = {{1,1,1,tactical::player_flag_commandable}};
        initial.units = {{1,1,1}};
        auto world = require(tactical::TacticalSession::create(initial));
        // Refused uses still belong to the log. Preloaded later input must
        // neither hide a turn-only tick nor leak into its canonical input.
        require(world.submit_object_turn({0,{1,0,tactical::AbilityKind::invulnerability}}));
        require(world.submit_object_turn({2,{1,1,tactical::AbilityKind::invulnerability}}));
        require(world.submit({{2,1,0},{1},tactical::StopPayload{}}));
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        net::TacticalSessionAdapter adapter(std::move(world), executor, {});
        std::vector<std::vector<std::uint8_t>> inputs, logs;
        std::vector<std::string> hashes;
        for (unsigned frame = 0; frame < 3; ++frame) {
            require(adapter.step());
            auto bytes = require(adapter.canonical_input());
            if (frame == 1) expect(bytes.empty(), "old and future object uses do not leak into the empty tick log");
            else {
                expect(!bytes.empty(), "turn-only canonical log is retained without a player batch");
                const auto replay = require(tactical::parse_replay(bytes));
                expect(replay.object_turns.size() == 1 && replay.object_turns.front().tick == frame,
                    "canonical adapter log carries only this tick's object use");
                expect(replay.commands.size() == (frame == 2 ? 1U : 0U),
                    "canonical adapter log carries ordinary input only on its execution tick");
            }
            inputs.push_back(std::move(bytes));
            hashes.push_back(adapter.state_hash());
            logs.push_back(require(tactical::write_replay(adapter.simulation().record())));
        }
        if (workers == 1) {
            input_reference = inputs; hash_reference = hashes; log_reference = logs;
        } else {
            expect(inputs == input_reference, "object-turn canonical inputs are identical across 1/2/4/8 workers");
            expect(logs == log_reference, "object-turn command logs are identical across 1/2/4/8 workers");
            expect(hashes == hash_reference, "object-turn state hashes are identical across 1/2/4/8 workers");
        }
        std::cout << "object-turn input workers=" << workers << " hash=" << hashes.back() << '\n';
    }
}

tactical::TacticalSetup setup() {
    tactical::TacticalSetup result;
    result.seed = 714;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    for (std::uint32_t player = 1; player <= 2; ++player) {
        tactical::UnitState unit;
        unit.entity_id = player;
        unit.type_id = 1;
        unit.owner = player;
        unit.position = {units(player == 1 ? -50 : 50), {}, {}};
        unit.rotation = player == 1 ? sim::math::identity_quat() : sim::math::Quat{{}, {}, units(1), {}};
        result.units.push_back(unit);
    }
    return result;
}

// Synthetic skirmish content, exercising movement, mutual target orders and weapon service.
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
    profile.type_id = 1;
    profile.category_bits = 1;
    profile.max_attack_distance = units(500);
    profile.weapons = {weapon};
    result.combat.profiles = {profile};
    return result;
}

std::vector<tactical::SeatBinding> bindings(std::uint32_t peers) {
    std::vector<tactical::SeatBinding> result;
    for (std::uint32_t peer = 0; peer < peers; ++peer)
        result.push_back({sim::SeatId{peer + 1}, peer % 2 + 1, true, false});
    return result;
}

struct Match {
    explicit Match(std::uint32_t peers, std::uint64_t delay = 3, std::uint64_t final_tick = 70,
        std::size_t workers = 1, net::SessionMonitor* monitor = nullptr, bool departures = false,
        bool keep_admission_tick_open = false,
        std::function<std::optional<std::uint64_t>(std::size_t)> pending_epoch = {},
        std::optional<eawr::testing::WanProfile> wan = {})
        : network(peers), executor(workers), roster(bindings(peers)) {
        net::SessionOptions options;
        options.input_delay = delay;
        options.final_tick_count = final_tick;
        options.completeness_timeout_ms = wan ? 100'000 : 100;
        if (wan) network.emulate(*wan);
        options.keep_admission_tick_open = keep_admission_tick_open;
        auto initial = setup();
        if (departures) initial.departure_policy = tactical::MultiplayerDeparturePolicy{{1, 2}};
        for (net::PeerIndex peer = 0; peer < peers; ++peer) {
            if (pending_epoch) options.pending_input_epoch = [pending_epoch, peer] { return pending_epoch(peer); };
            transports.push_back(std::make_unique<FakeTransport>(network, peer));
            sessions.push_back(require(net::TacticalLockstepSession::create(
                initial, content(), *transports.back(), executor,
                roster, options, peer == 0 ? monitor : nullptr)));
        }
    }
    FakeNetwork network;
    eawr::platform::ThreadWorkerAdapter executor;
    std::vector<tactical::SeatBinding> roster;
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<net::TacticalLockstepSession> sessions;
};

std::uint64_t batch_tick(std::span<const std::uint8_t> frame) {
    std::uint64_t tick{};
    for (unsigned index = 0; index < 8; ++index) tick |= static_cast<std::uint64_t>(frame[5 + index]) << (8U * index);
    return tick;
}

std::string adaptive_agreement(std::size_t workers, std::uint32_t peers, bool skewed) {
    FakeNetwork network(peers);
    eawr::platform::ThreadWorkerAdapter executor(workers);
    const auto roster = bindings(peers);
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<std::unique_ptr<net::TacticalSessionAdapter>> adapters;
    std::vector<net::LockstepSession> sessions;
    std::vector<std::vector<std::string>> hashes(peers);
    std::vector<std::vector<net::SeatInput>> accepted(peers);
    std::vector<std::uint64_t> submitted(peers, 500);
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> changes(peers);
    std::vector<std::vector<std::vector<std::uint8_t>>> controls(peers);
    for (net::PeerIndex peer = 0; peer < peers; ++peer) {
        transports.push_back(std::make_unique<FakeTransport>(network, peer));
        net::DesyncOptions monitor_options;
        monitor_options.input_delay = 4;
        monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), monitor_options));
        adapters.push_back(std::make_unique<net::TacticalSessionAdapter>(
            require(tactical::TacticalSession::create(setup(), content())), executor, roster));
        net::SessionOptions options;
        options.input_delay = 4;
        options.final_tick_count = 500;
        options.non_lan = true;
        options.retain_timing_event = [&, peer](auto tick, auto bytes) {
            expect(tick > adapters[peer]->completed_tick(), "agreed timing retained before activation");
            controls[peer].emplace_back(bytes.begin(), bytes.end());
            return eawr::core::Result<void>::success();
        };
        options.adaptive_timing = net::AdaptiveTiming{0, true, [&, peer](net::PeerIndex remote) {
            expect(peer == 0, "only the host samples RTT");
            // Worst of all peers, with samples changing mid-match in both directions.
            return std::optional<std::uint64_t>{adapters[0]->completed_tick() < 200 && remote == peers - 1 ? 330U : 0U};
        }};
        std::vector<net::PeerSeat> seats;
        for (const auto& bound : roster) seats.push_back({bound.seat, true});
        sessions.push_back(require(net::LockstepSession::create(*adapters.back(), *transports.back(), seats,
            options, monitors.back().get())));
        sessions.back().set_stage_observer([&, peer](auto, const net::SeatInput& input, auto) {
            accepted[peer].push_back(input);
        });
    }
    network.fault = [skewed](auto from, auto to, auto frame) {
        FakeDelivery action;
        if (skewed && from == 0 && to == 1 && frame[0] == net::timing_proposal_kind) action.ordered_delay = 7;
        if (skewed && from == 1 && to == 0 && frame[0] == net::timing_ready_kind) action.ordered_delay = 5;
        if (frame[0] == net::timing_proposal_kind || frame[0] == net::timing_ready_kind) action.duplicate = true;
        return action;
    };
    for (std::uint64_t round = 0; round < 3000; ++round) {
        network.round = round;
        for (net::PeerIndex peer = 0; peer < peers; ++peer) {
            const auto tick = adapters[peer]->completed_tick();
            if (tick < 400 && submitted[peer] != tick) {
                require(sessions[peer].submit([&](auto key) {
                    return net::encode_tactical_input(key, roster[peer], {{roster[peer].player},
                        tactical::MovePayload{{units(static_cast<std::int64_t>(tick % 20)), {}, {}}}});
                }));
                submitted[peer] = tick;
            }
            const auto old_delay = sessions[peer].input_delay();
            const auto result = require(sessions[peer].pump(round));
            if (sessions[peer].input_delay() != old_delay)
                changes[peer].push_back({tick, sessions[peer].input_delay()});
            if (result.completed) hashes[peer].push_back(result.completed->state_sha256);
        }
        if (std::all_of(monitors.begin(), monitors.end(), [](const auto& monitor) { return monitor->agreed_end(); })) break;
    }
    for (net::PeerIndex peer = 0; peer < peers; ++peer) {
        expect(adapters[peer]->completed_tick() == 500 && monitors[peer]->agreed_end(), "adaptive monitored match completes");
        expect(changes[peer] == changes[0] && changes[peer].size() == 2,
            "every peer applies increase and decrease at exactly the same ticks");
        expect(controls[peer] == controls[0] && controls[peer].size() == 2,
            "reconnect receives each immutable agreed control exactly once on every peer");
        expect(hashes[peer] == hashes[0], "every adaptive completed hash agrees");
        expect(adapters[peer]->simulation().record() == adapters[0]->simulation().record(), "adaptive replay commands agree");
        expect(adapters[peer]->simulation().record().commands.size() == peers * 400, "transition loses no in-flight command");
        std::sort(accepted[peer].begin(), accepted[peer].end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        expect(accepted[peer] == accepted[0], "sealed entry stamps and opaque bytes agree without restamping");
        expect(sessions[peer].input_delay() == 4 && sessions[peer].input_horizon() == net::sealed_input_horizon(4),
            "active horizon contracts after immutable transition drains");
    }
    expect(changes[0] == std::vector<std::pair<std::uint64_t, std::uint64_t>>{{165, 8}, {293, 4}},
        "MP-05 tick cadence and beyond-horizon effective ticks");
    return adapters[0]->state_hash();
}

void adaptive_policy_and_refusals() {
    expect(net::adaptive_input_delay(0) == 4 && net::adaptive_input_delay(0, false) == 6, "MP-05 latency floors");
    expect(net::adaptive_input_delay(330) == 8 && net::adaptive_input_delay(UINT64_MAX) == 64, "MP-05 rounding and overflow-safe cap");
    for (const auto kind : {net::timing_proposal_kind, net::timing_ready_kind}) {
        Match match(2);
        std::array<std::uint8_t, 17> frame{};
        frame[0] = kind; frame[1] = 40; frame[9] = 8;
        match.network.deliver(1, 0, frame);
        match.network.round = 1;
        const auto result = match.sessions[0].pump(1);
        expect(!result && result.error().code == net::timing_errors::agreement, "fixed session refuses timing traffic");
    }
}

std::string agreement(std::uint32_t peers, std::uint64_t delay, std::size_t workers, const std::string& fault = {},
    std::optional<eawr::testing::WanProfile> wan = {}) {
    Match match(peers, delay, 70, workers, nullptr, false, false, {}, wan);
    if (!fault.empty()) match.network.fault = [fault](auto from, auto to, auto frame) {
        FakeDelivery result;
        if (from == 0 && to == 1 && batch_tick(frame) == 4) {
            if (fault == "duplicate") result.duplicate = true;
            if (fault == "late_duplicate") { result.duplicate = true; result.duplicate_delay = 12; }
            if (fault == "late") result.ordered_delay = 9;
        }
        return result;
    };
    std::vector<unsigned> script(peers);
    std::vector<std::vector<std::string>> hashes(peers);
    bool waited = false;
    for (std::uint64_t round = 0; round < (wan ? 30000U : 500U); ++round) {
        match.network.round = round;
        for (net::PeerIndex peer = 0; peer < peers; ++peer) {
            auto& session = match.sessions[peer];
            const auto completed = session.simulation().completed_tick();
            const auto owner = match.roster[peer].player;
            const auto opponent = owner == 1 ? 2U : 1U;
            if (script[peer] == 0) {
                const auto key = require(session.submit({{owner}, tactical::AttackPayload{opponent}}));
                expect(key.tick == delay && key.seat == match.roster[peer].seat, "initial stamp and bound seat");
                ++script[peer];
            } else if (script[peer] == 1 && completed >= 15) {
                require(session.submit({{owner}, tactical::MovePayload{{units(owner == 1 ? -20 : 20), units(10), {}}}}));
                ++script[peer];
            } else if (script[peer] == 2 && completed >= 30) {
                require(session.submit({{owner}, tactical::AttackPayload{opponent}}));
                ++script[peer];
            }
            auto result = require(session.pump(round));
            if (result.completed) hashes[peer].push_back(result.completed->state_sha256);
            waited = waited || session.waiting_for().has_value();
        }
        if (std::all_of(match.sessions.begin(), match.sessions.end(),
            [](const auto& session) { return session.simulation().completed_tick() == 70; })) break;
    }
    const auto label = std::to_string(peers) + " peers, D=" + std::to_string(delay)
        + ", workers=" + std::to_string(workers) + ", fault=" + fault;
    for (net::PeerIndex peer = 0; peer < peers; ++peer) {
        expect(match.sessions[peer].simulation().completed_tick() == 70, label + " completes");
        expect(hashes[peer] == hashes[0], label + " every completed hash agrees");
        expect(match.sessions[peer].simulation().record() == match.sessions[0].simulation().record(), label + " replay agrees");
    }
    expect(waited, label + " exercises completeness waits");
    const auto recording = match.sessions[0].simulation().record();
    expect(recording.commands.size() == peers * 3, label + " commands from every seat");
    for (const auto& command : recording.commands)
        expect(command.key.tick == delay || command.key.tick == 15 + delay || command.key.tick == 30 + delay,
            label + " commands retain their entry stamp");
    using Peer = lockstep::Peer<lockstep::TacticalAdapter>;
    auto split = lockstep::split_by_issuer<lockstep::TacticalAdapter>(recording.commands, peers);
    std::vector<Peer> harness;
    std::vector<Peer*> pointers;
    for (net::PeerIndex peer = 0; peer < peers; ++peer)
        harness.push_back({require(tactical::TacticalSession::create(setup(), content())), &match.executor,
            std::move(split[peer]), {}, {}});
    for (auto& peer : harness) pointers.push_back(&peer);
    lockstep::Options options;
    options.input_delay = delay;
    options.final_tick = 70;
    const auto report = lockstep::run<lockstep::TacticalAdapter>(pointers, options);
    expect(report.agreed(), label + " harness agrees: " + report.describe());
    expect(report.final_hash == match.sessions[0].simulation().state_sha256(), label + " harness/core final hash equal");
    return match.sessions[0].simulation().state_sha256();
}

void missing_and_faults() {
    for (const std::string fault : {"loss", "reorder"}) {
        Match match(2);
        match.network.fault = [fault](auto from, auto to, auto frame) {
            FakeDelivery result;
            if (from == 0 && to == 1 && batch_tick(frame) == 0) {
                result.drop = fault == "loss";
                result.reorder_delay = fault == "reorder" ? 10U : 0U;
            }
            return result;
        };
        bool failed = false;
        for (std::uint64_t round = 0; round < 20 && !failed; ++round) {
            match.network.round = round;
            for (auto& session : match.sessions) {
                auto pumped = session.pump(round);
                if (!pumped) {
                    expect(pumped.error().code == net::session_errors::stream_gap, fault + " typed stream gap");
                    const auto repeated = session.pump(round + 1);
                    expect(!repeated && repeated.error().code == pumped.error().code, "failure is latched");
                    failed = true;
                }
            }
        }
        expect(failed, fault + " fails loudly");
        expect(match.sessions[1].simulation().completed_tick() == 0, "gap never becomes empty input");
    }
    Match missing(2, 3, 1);
    missing.network.fault = [](auto from, auto to, auto) {
        FakeDelivery action;
        action.drop = from == 0 && to == 1;
        return action;
    };
    for (auto& session : missing.sessions) require(session.pump(7));
    missing.network.round = 1;
    require(missing.sessions[1].pump(10));
    const auto& waiting = missing.sessions[1].waiting_for();
    expect(waiting && waiting->tick == 0 && waiting->peers == std::vector<net::PeerIndex>{0}
        && waiting->since_ms == 7, "missing empty batch reports exact peer, tick and stable start time");
    expect(missing.sessions[1].simulation().completed_tick() == 0, "missing empty batch blocks");
    auto expired = missing.sessions[1].pump(107);
    expect(!expired && expired.error().code == net::session_errors::stalled, "loss of final frame fails by caller timeout");

    Match empty(2, 3, 1);
    for (std::uint64_t round = 0; round < 3; ++round) {
        empty.network.round = round;
        for (auto& session : empty.sessions) require(session.pump(round));
    }
    for (const auto& session : empty.sessions)
        expect(session.simulation().completed_tick() == 1 && !session.waiting_for(), "explicit empty batches complete");
}

void strict_codec_and_admission() {
    const auto binding = bindings(2)[0];
    const auto input = [&](std::uint64_t tick, std::uint64_t sequence, net::LocalIntent intent) {
        sim::SeatCommandKey key{tick, binding.seat, sequence};
        return net::SeatInput{key, require(net::encode_tactical_input(key, binding, intent))};
    };
    const auto stamped = input(3, 0, {{1}, tactical::AttackPayload{2}});
    net::SealedBatch batch{0, 3, 3, {stamped}};
    const auto encoded = require(net::encode_batch(batch));
    const auto decoded = require(net::decode_batch(encoded));
    expect(decoded.commands == batch.commands, "opaque seat-input codec roundtrip");
    const auto tactical_command = require(net::decode_tactical_input(decoded.commands.front(), binding));
    expect(tactical_command.key == stamped.key && tactical_command.player == 1, "existing replay command codec roundtrip");
    for (std::size_t size = 0; size < encoded.size(); ++size)
        expect(!net::decode_batch(std::span(encoded).first(size)), "every truncated batch prefix refused");
    for (std::size_t size = 0; size < stamped.bytes.size(); ++size) {
        auto truncated = stamped;
        truncated.bytes.resize(size);
        expect(!net::decode_tactical_input(truncated, binding), "every truncated replay command prefix refused");
    }
    auto corrupt = encoded;
    corrupt.push_back(0);
    expect(!net::decode_batch(corrupt), "trailing bytes refused");
    for (const auto offset : {0U, 13U, 21U, 37U}) {
        corrupt = encoded;
        corrupt[offset] ^= 255;
        expect(!net::decode_batch(corrupt), "invalid kind/sequence/count/length refused");
    }
    auto wrong_stamp = stamped;
    ++wrong_stamp.key.tick;
    expect(!net::decode_tactical_input(wrong_stamp, binding), "opaque command cannot be restamped");
    expect(!net::encode_tactical_input(stamped.key, binding, {{1}, tactical::DamagePayload{}}), "engine payload refused by adapter encoder");
    auto engine_replay = require(tactical::parse_replay(stamped.bytes));
    engine_replay.commands.front().payload = tactical::DamagePayload{units(1)};
    net::SeatInput hostile{stamped.key, require(tactical::write_replay(engine_replay))};
    auto unauthorized = net::decode_tactical_input(hostile, binding);
    expect(!unauthorized && unauthorized.error().code == net::session_errors::admission,
        "valid replay encoding cannot smuggle engine-only input across the wire");
    expect(!net::encode_tactical_input(stamped.key, binding, {{1}, tactical::BuyPayload{1, true}}),
        "prepaid buy engine route refused");
    expect(!net::encode_tactical_input(stamped.key, binding, {{}, tactical::ReinforcePayload{1, {}, 8}}),
        "pooled reinforcement engine route refused");
    expect(!net::encode_tactical_input(stamped.key, binding,
        {{1}, tactical::SpecialFirePayload{2, tactical::SpecialFireAdmission::ai_object}}),
        "AI special-fire engine route refused");
    Match match(2);
    const auto before = require(match.sessions[0].submit({{1}, tactical::StopPayload{}}));
    expect(before.tick == 3 && before.sequence == 0, "stamp at entry before seal");
    auto engine = match.sessions[0].submit({{1}, tactical::DamagePayload{}});
    expect(!engine && engine.error().code == net::session_errors::admission, "local engine payload refused");
    require(match.sessions[0].pump(0));
    const auto after = require(match.sessions[0].submit({{1}, tactical::StopPayload{}}));
    expect(after.tick == 4 && after.sequence == 1, "sealed batch immutable and rejected input consumes no sequence");
    Match spoof(2);
    spoof.network.inbox[1].push_back({0, {0, require(net::encode_batch({1, 0, 0, {}}))}});
    auto rejected = spoof.sessions[1].pump(0);
    expect(!rejected && rejected.error().code == net::session_errors::admission, "authenticated stream sender checked");
    Match wrong_seat(2);
    const auto remote_key = sim::SeatCommandKey{0, sim::SeatId{2}, 0};
    net::SeatInput impersonated{remote_key, require(net::encode_tactical_input(remote_key, bindings(2)[1],
        {{2}, tactical::StopPayload{}}))};
    wrong_seat.network.inbox[1].push_back({0, {0, require(net::encode_batch({0, 0, 0, {impersonated}}))}});
    rejected = wrong_seat.sessions[1].pump(0);
    expect(!rejected && rejected.error().code == net::session_errors::admission, "wire seat cannot impersonate another peer");
    Match duplicate(2);
    auto empty = require(net::encode_batch({0, 0, 0, {}}));
    auto changed = require(net::encode_batch({0, 0, 0, {input(0, 0, {{1}, tactical::StopPayload{}})}}));
    duplicate.network.inbox[1].push_back({0, {0, empty}});
    duplicate.network.inbox[1].push_back({0, {0, changed}});
    rejected = duplicate.sessions[1].pump(0);
    expect(!rejected && rejected.error().code == net::session_errors::conflicting_duplicate, "conflicting duplicate refused");
    Match sequence(2);
    for (std::uint64_t tick = 0; tick < 2; ++tick)
        sequence.network.inbox[1].push_back({0, {0, require(net::encode_batch(
            {0, tick, tick, {input(tick, 4, {{1}, tactical::StopPayload{}})}}))}});
    rejected = sequence.sessions[1].pump(0);
    expect(!rejected && rejected.error().code == net::session_errors::admission, "seat sequence replay across ticks refused");
    Match oversized(2);
    oversized.network.inbox[1].push_back({0, {0, std::vector<std::uint8_t>(net::max_batch_bytes + 1, 16)}});
    rejected = oversized.sessions[1].pump(0);
    expect(!rejected && rejected.error().code == net::session_errors::malformed, "oversize received frame refused before decode");
}

class Monitor : public net::SessionMonitor {
public:
    bool allowed{}, hold_end{}, is_frozen{};
    std::uint64_t final_tick{3};
    std::vector<net::PeerIndex> missing_voters;
    std::vector<std::uint64_t> ticks;
    std::vector<std::vector<std::uint8_t>> inputs;
    eawr::core::Result<void> record(std::uint64_t tick, std::string_view hash,
        std::span<const std::uint8_t> state, std::span<const std::uint8_t> input, bool end) override {
        expect(end == (tick == final_tick), "monitor receives final flag");
        expect(!hash.empty() && state.empty() == (tick != 0 && !end), "monitor gets encoding only when requested");
        if (end && hold_end) allowed = false;
        ticks.push_back(tick);
        inputs.emplace_back(input.begin(), input.end());
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<void> receive(const net::Received& received) override {
        expect(received.frame.front() == 32, "desync frame routed");
        allowed = true;
        return eawr::core::Result<void>::success();
    }
    bool needs_encoding(std::uint64_t tick, bool end) const override { return tick == 0 || end; }
    bool frozen() const override { return is_frozen; }
    bool may_step() const override { return allowed; }
    std::vector<net::PeerIndex> waiting_for_peers() const override { return missing_voters; }
};

std::string lan_click_latency(std::uint64_t delay, std::size_t workers, unsigned handoff) {
    namespace ui = eawr::presentation::ui;
    Monitor gate; gate.final_tick = 24;
    std::array<std::unique_ptr<ui::CommandScheduler>, 2> schedulers;
    Match match(2, delay, 24, workers, &gate, false, true, [&](std::size_t peer) {
        return schedulers[peer] ? schedulers[peer]->pending_intent_epoch() : std::nullopt;
    });
    std::array<unsigned, 2> seals{}, receives{};
    std::array<std::array<unsigned, 2>, 2> executions{};
    for (std::size_t peer = 0; peer < 2; ++peer) {
        schedulers[peer] = std::make_unique<ui::CommandScheduler>(
            static_cast<tactical::PlayerId>(peer + 1), 0, 0,
            [&, peer] { return match.sessions[peer].simulation().completed_tick(); });
        match.sessions[peer].set_stage_observer(
            [&, peer](std::string_view stage, const net::SeatInput& input, std::uint64_t frame) {
                const auto click = input.key.sequence == 0 ? 0U : 12U;
                expect(input.key.tick == click + delay, "LAN stage retains agreed admission tick");
                if (stage == "seal") {
                    ++seals[peer];
                    expect(frame == click + (handoff == 2 ? std::min<std::uint64_t>(2, delay) : 1),
                        "LAN seals the reserved slot after its producer handoff");
                } else if (stage == "receive") ++receives[peer];
            });
    }
    std::uint64_t round{};
    for (const std::uint64_t click : {0U, 12U}) {
        // Revisit an admission epoch without advancing it, as loading/hash and
        // completeness waits do. A visited UI epoch is still an open LAN intent.
        gate.allowed = false;
        for (std::size_t peer = 0; peer < 2; ++peer)
            expect(schedulers[peer]->take(click).empty(), "LAN empty admission visit");
        for (unsigned stalled = 0; stalled < 4; ++stalled) {
            match.network.round = round;
            require(match.sessions[0].pump(round++));
        }
        std::array<std::optional<ui::CommandScheduler::IntentEpochScope>, 2> scopes;
        if (handoff == 2) {
            for (std::size_t peer = 0; peer < 2; ++peer) scopes[peer].emplace(schedulers[peer]->capture_intent_epoch());
            gate.allowed = true;
            const auto decoded_frame = click + std::min<std::uint64_t>(2, delay);
            for (unsigned pump = 0; pump < 100; ++pump) {
                match.network.round = round;
                for (auto& session : match.sessions)
                    if (session.simulation().completed_tick() < decoded_frame) require(session.pump(round));
                ++round;
                if (std::all_of(match.sessions.begin(), match.sessions.end(), [decoded_frame](const auto& session) {
                    return session.simulation().completed_tick() == decoded_frame;
                })) break;
            }
            gate.allowed = false;
        }
        for (std::size_t peer = 0; peer < 2; ++peer) {
            expect(match.sessions[peer].simulation().completed_tick()
                == click + (handoff == 2 ? std::min<std::uint64_t>(2, delay) : 0), "LAN input decoding frame");
            expect(schedulers[peer]->open_tick() == click, "LAN producer stamps the current live epoch");
            const auto nested = schedulers[peer]->capture_intent_epoch();
            expect(nested.epoch() == click, "nested input decoder retains its original producer epoch");
            ui::TacticalIntent intent;
            intent.verb = ui::TacticalVerb::move;
            intent.units = {peer + 1};
            intent.destination = {units(click == 0 ? 20 : -20), units(10), {}};
            expect(static_cast<bool>(schedulers[peer]->issue(intent)), "LAN click accepted by production UI sink");
        }
        for (auto& scope : scopes) scope.reset();
        if (handoff == 1) {
            gate.allowed = true;
            for (unsigned pump = 0; pump < 100; ++pump) {
                match.network.round = round;
                for (auto& session : match.sessions)
                    if (session.simulation().completed_tick() == click) require(session.pump(round));
                ++round;
                if (std::all_of(match.sessions.begin(), match.sessions.end(), [click](const auto& session) {
                    return session.simulation().completed_tick() == click + 1;
                })) break;
            }
            gate.allowed = false;
        }
        // Revisit the already advanced frame after decoding ended but before
        // take: the queued intent must still reserve its original input slot.
        for (std::size_t peer = 0; peer < 2; ++peer) {
            expect(schedulers[peer]->pending_intent_epoch() == click, "queued intent retains the input reservation");
        }
        for (unsigned visit = 0; visit < 4; ++visit) {
            match.network.round = round;
            require(match.sessions[0].pump(round++));
        }
        for (std::size_t peer = 0; peer < 2; ++peer) {
            const auto frame = click + (handoff == 2 ? std::min<std::uint64_t>(2, delay) : handoff);
            expect(match.sessions[peer].simulation().completed_tick() == frame,
                "LAN drain visits the requested completed frame");
            auto commands = schedulers[peer]->take(frame);
            expect(commands.size() == 1 && commands.front().key.tick == click,
                "LAN drain preserves the producer epoch");
            if (commands.empty()) continue;
            const auto future = match.sessions[peer].submit(
                {commands.front().units, commands.front().payload}, frame + 1);
            expect(!future && future.error().code == net::session_errors::admission,
                "LAN rejects a future producer epoch without consuming its sequence");
            const auto key = require(match.sessions[peer].submit(
                {commands.front().units, commands.front().payload}, commands.front().key.tick));
            expect(key.tick == click + delay, "LAN submit reserves click plus configured delay");
            expect(key.sequence == (click == 0 ? 0U : 1U), "rejected producer epoch consumes no sequence");
        }
        gate.allowed = true;
        const auto stop = click + 12;
        for (unsigned pump = 0; pump < 100; ++pump) {
            match.network.round = round;
            for (std::size_t peer = 0; peer < 2; ++peer) {
                if (match.sessions[peer].simulation().completed_tick() >= stop) continue;
                const auto result = require(match.sessions[peer].pump(round));
                if (!result.completed) continue;
                const auto record = match.sessions[peer].simulation().record();
                for (const auto& command : record.commands) {
                    if (command.key.tick + 1 != result.completed->completed_tick) continue;
                    const auto origin = static_cast<std::size_t>(command.key.player_id - 1);
                    const auto wave = static_cast<std::size_t>(command.key.sequence);
                    expect(origin < 2 && wave < 2, "LAN latency command identity");
                    if (origin >= 2 || wave >= 2) continue;
                    ++executions[peer][origin];
                    const auto click_frame = wave == 0 ? 0U : 12U;
                    expect(result.completed->completed_tick - click_frame == delay + 1,
                        "LAN click to completed execution equals input delay plus one");
                    std::cout << "LAN latency workers=" << workers << " delay=" << delay
                        << " handoff=" << handoff << " peer=" << peer
                        << " click=" << click_frame << " target=" << command.key.tick
                        << " execute=" << result.completed->completed_tick << '\n';
                }
            }
            ++round;
            if (std::all_of(match.sessions.begin(), match.sessions.end(), [stop](const auto& session) {
                return session.simulation().completed_tick() == stop;
            })) break;
        }
        expect(match.sessions[0].simulation().completed_tick() == stop
            && match.sessions[1].simulation().completed_tick() == stop, "LAN latency loopback advances both peers");
    }
    for (std::size_t peer = 0; peer < 2; ++peer) {
        expect(seals[peer] == 2 && receives[peer] == 2, "LAN latency stamps both origins on both peers");
        expect(executions[peer] == std::array<unsigned, 2>{2, 2}, "LAN executes every click exactly once on each peer");
    }
    expect(match.sessions[0].simulation().record() == match.sessions[1].simulation().record(),
        "LAN admission ticks and replay commands agree across peers");
    expect(match.sessions[0].simulation().state_sha256() == match.sessions[1].simulation().state_sha256(),
        "LAN latency peers finish at the same state");
    return match.sessions[0].simulation().state_sha256();
}

void monitor_quit_and_options() {
    Monitor monitor;
    Match match(2, 1, 3, 1, &monitor);
    unsigned departure_callbacks{};
    match.sessions[0].set_departure_handler([&](std::span<const sim::SeatCommandKey> keys) {
        expect(match.sessions[0].simulation().completed_tick() == 2 && keys.size() == 1,
            "departure policy callback runs at frame tail");
        ++departure_callbacks;
        return eawr::core::Result<void>::success();
    });
    require(match.sessions[0].submit({{}, tactical::QuitPayload{}}));
    for (auto& session : match.sessions) require(session.pump(0));
    match.network.round = 1;
    for (auto& session : match.sessions) require(session.pump(1));
    expect(match.sessions[0].simulation().completed_tick() == 0, "monitor exchange blocks stepping");
    expect(monitor.ticks == std::vector<std::uint64_t>{0}, "tick-zero recorded once");
    match.network.inbox[0].push_back({1, {1, {32}}});
    bool saw_quit = false;
    for (std::uint64_t round = 2; round < 20; ++round) {
        match.network.round = round;
        for (auto& session : match.sessions) {
            auto result = require(session.pump(round));
            if (!result.quits.empty()) {
                expect(result.completed && result.completed->completed_tick == 2, "quit reported at admitted frame tail");
                expect(result.quits.front().tick == 1 && result.quits.front().seat == sim::SeatId{1}, "quit keeps seat stamp");
                saw_quit = true;
            }
        }
    }
    expect(saw_quit && monitor.ticks == std::vector<std::uint64_t>({0, 1, 2, 3}), "monitor includes final tick and quit hook");
    expect(departure_callbacks == 1, "departure callback delivered exactly once");
    expect(monitor.inputs.size() == 4 && !monitor.inputs[2].empty() && monitor.inputs[1].empty(),
        "monitor receives replay-encoded canonical commands and explicit empty input");
    monitor.allowed = false;
    const auto final_hash = match.sessions[0].simulation().state_sha256();
    require(match.sessions[0].pump(20));
    expect(match.sessions[0].simulation().state_sha256() == final_hash, "frozen final pump receives without stepping");
    Match clock(2);
    require(clock.sessions[0].pump(10));
    auto backwards = clock.sessions[0].pump(9);
    expect(!backwards && backwards.error().code == net::session_errors::clock, "monotonic caller clock enforced");
    Match closed(2);
    closed.network.open[1] = false;
    auto lost = closed.sessions[0].pump(0);
    expect(!lost && lost.error().code == net::session_errors::link_closed, "closed required stream typed error");
    Match delivered(2, 1, 3);
    for (auto& session : delivered.sessions) require(session.pump(0));
    delivered.network.round = 1;
    delivered.network.open[0] = false;
    auto completed = require(delivered.sessions[1].pump(1));
    expect(completed.completed && completed.completed->completed_tick == 1, "closed-after-delivery still completes tick zero");
    completed = require(delivered.sessions[1].pump(2));
    expect(completed.completed && completed.completed->completed_tick == 2, "send to closed peer does not discard ready input");
    auto incomplete = delivered.sessions[1].pump(3);
    expect(!incomplete && incomplete.error().code == net::session_errors::link_closed,
        "closed stream is typed only when its next batch is missing");
    Monitor gated_monitor;
    gated_monitor.missing_voters = {1};
    Match gated(2, 1, 3, 1, &gated_monitor);
    for (auto& session : gated.sessions) require(session.pump(0));
    gated.network.round = 1;
    require(gated.sessions[0].pump(1)); // Delivered inputs are present, summary still missing.
    gated.network.open[1] = false;
    auto gated_closed = gated.sessions[0].pump(2);
    expect(!gated_closed && gated_closed.error().code == net::session_errors::link_closed,
        "default monitor gate diagnoses drained closed voter even with complete tick input");
    for (const bool opt_in : {false, true}) for (const bool close_link : {false, true}) {
        Monitor terminal_monitor;
        terminal_monitor.allowed = true; terminal_monitor.hold_end = true;
        terminal_monitor.missing_voters = {1};
        Match terminal(2, 1, 3, 1, &terminal_monitor, opt_in);
        for (std::uint64_t round = 0; round < 10; ++round) {
            terminal.network.round = round;
            for (auto& session : terminal.sessions) require(session.pump(round));
        }
        expect(terminal.sessions[0].simulation().completed_tick() == 3, "terminal monitor exchange starts after final tick");
        terminal.network.open[1] = !close_link;
        auto terminal_result = terminal.sessions[0].pump(close_link ? 10 : 110);
        expect(!terminal_result && terminal_result.error().code == (close_link
            ? net::session_errors::link_closed : net::session_errors::stalled),
            "terminal hash barrier bounds closed or silent voters in both departure modes");
    }
    Monitor real_freeze;
    real_freeze.is_frozen = true; real_freeze.missing_voters = {1};
    Match frozen_departure(2, 1, 3, 1, &real_freeze, true);
    for (auto& session : frozen_departure.sessions) require(session.pump(0));
    require(frozen_departure.sessions[0].pump(1000));
    expect(!frozen_departure.sessions[0].failure() && frozen_departure.sessions[0].departure_outcomes().empty(),
        "real desync freeze never expires a missing voter into a departure");
    Match final_delivery(2, 1, 3);
    for (auto& session : final_delivery.sessions) require(session.pump(0));
    final_delivery.network.round = 1;
    require(final_delivery.sessions[0].pump(1));
    final_delivery.network.round = 2;
    require(final_delivery.sessions[0].pump(2)); // seals its final batch before disconnecting
    final_delivery.network.round = 3;
    final_delivery.network.open[0] = false;
    for (std::uint64_t now = 3; now < 7; ++now) require(final_delivery.sessions[1].pump(now));
    expect(final_delivery.sessions[1].simulation().completed_tick() == 3,
        "all delivered final batches complete after sender disconnects");
    Match backlog(2, 1, 2);
    for (auto& session : backlog.sessions) require(session.pump(0));
    auto& queued = backlog.network.inbox[1];
    const auto duplicate_zero = queued.front();
    queued.insert(queued.begin() + 1, 600, duplicate_zero);
    backlog.network.round = 1;
    backlog.network.open[0] = false;
    require(backlog.sessions[1].pump(1));
    require(backlog.sessions[1].pump(2));
    require(backlog.sessions[1].pump(3));
    expect(backlog.sessions[1].simulation().completed_tick() == 2,
        "bounded polls drain delivered final input behind a large duplicate backlog after closure");
    Match duplicate_loss(2, 1, 2);
    duplicate_loss.network.fault = [](auto from, auto to, auto frame) {
        FakeDelivery action;
        action.drop = from == 0 && to == 1 && batch_tick(frame) == 1;
        return action;
    };
    for (auto& session : duplicate_loss.sessions) require(session.pump(0));
    auto& repeated = duplicate_loss.network.inbox[1];
    const auto duplicate_prefix = repeated.front();
    repeated.insert(repeated.end(), 1000, duplicate_prefix);
    duplicate_loss.network.round = 1;
    require(duplicate_loss.sessions[1].pump(1));
    require(duplicate_loss.sessions[1].pump(2));
    auto flooded = duplicate_loss.sessions[1].pump(102);
    expect(!flooded && flooded.error().code == net::session_errors::stalled,
        "loss expiry remains active when duplicate traffic exhausts the poll budget");
    Match options(2);
    auto roster = options.roster;
    roster[1].seat = roster[0].seat;
    auto invalid = net::TacticalLockstepSession::create(setup(), content(),
        *options.transports[0], options.executor, roster);
    expect(!invalid && invalid.error().code == net::session_errors::options, "duplicate seat refused");
    Monitor frozen;
    Match blocked(2, 1, 3, 1, &frozen);
    blocked.network.fault = [](auto from, auto to, auto) {
        FakeDelivery action;
        action.drop = from == 1 && to == 0;
        return action;
    };
    for (auto& session : blocked.sessions) require(session.pump(0));
    require(blocked.sessions[0].pump(1000));
    expect(!blocked.sessions[0].failure() && blocked.sessions[0].simulation().completed_tick() == 0,
        "desync freeze keeps receiving without turning into a completeness timeout");
    expect(!blocked.sessions[0].waiting_for(), "monitor freeze has no active completeness clock");
    frozen.allowed = true;
    require(blocked.sessions[0].pump(1001));
    expect(blocked.sessions[0].waiting_for() && blocked.sessions[0].waiting_for()->since_ms == 1001,
        "completeness clock starts fresh after unfreeze");
    require(blocked.sessions[0].pump(1099));
    auto resumed_timeout = blocked.sessions[0].pump(1101);
    expect(!resumed_timeout && resumed_timeout.error().code == net::session_errors::stalled,
        "only unfrozen completeness time reaches timeout");
    Monitor zero_monitor;
    zero_monitor.final_tick = 0;
    Match zero(2, 1, 0, 1, &zero_monitor);
    require(zero.sessions[0].pump(0));
    require(zero.sessions[0].pump(1));
    expect(zero_monitor.ticks == std::vector<std::uint64_t>{0}, "zero-length session records one final tick-zero observation");
}

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

class DepartureGate : public Monitor {
public:
    std::vector<std::pair<net::PeerIndex, std::uint64_t>> settled;
    eawr::core::Result<void> on_departure(net::PeerIndex peer, std::uint64_t cutoff) override {
        settled.emplace_back(peer, cutoff);
        return eawr::core::Result<void>::success();
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

void adaptive_settled_departure(bool delayed_host_ack = false, bool delayed_other_ack = false) {
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

std::string chained_tactical_departures(unsigned workers, bool settling = false) {
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

std::string retained_timing_departure(unsigned workers, bool host_leaves, bool late_ready = false,
    bool double_loss = false, bool expire = false) {
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
