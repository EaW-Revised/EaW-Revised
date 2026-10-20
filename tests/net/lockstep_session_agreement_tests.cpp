#include "../support/check.hpp"
#include "lockstep_session_fixtures.hpp"
#include "fake_transport.hpp"
#include "eawr/net/tactical_adapter.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/ui/command_sink.hpp"
#include "../support/lockstep_loopback.hpp"
#include "eawr/net/lockstep_session.hpp"
#include "eawr/net/session_monitor.hpp"
#include "eawr/net/tactical_input.hpp"
#include "eawr/net/transport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
namespace lockstep = eawr::testing::lockstep;
using eawr::testing::FakeDelivery;
using eawr::testing::FakeNetwork;
using eawr::testing::FakeTransport;
using sim::math::Fixed;

eawr::core::Result<void> Monitor::record(std::uint64_t tick, std::string_view hash,
        std::span<const std::uint8_t> state, std::span<const std::uint8_t> input, bool end) {
        expect(end == (tick == final_tick), "monitor receives final flag");
        expect(!hash.empty() && state.empty() == (tick != 0 && !end), "monitor gets encoding only when requested");
        if (end && hold_end) allowed = false;
        ticks.push_back(tick);
        inputs.emplace_back(input.begin(), input.end());
        return eawr::core::Result<void>::success();
    }

eawr::core::Result<void> Monitor::receive(const net::Received& received) {
        expect(received.frame.front() == 32, "desync frame routed");
        allowed = true;
        return eawr::core::Result<void>::success();
    }

bool Monitor::needs_encoding(std::uint64_t tick, bool end) const { return tick == 0 || end; }

bool Monitor::frozen() const { return is_frozen; }

bool Monitor::may_step() const { return allowed; }

std::vector<net::PeerIndex> Monitor::waiting_for_peers() const { return missing_voters; }

eawr::core::Result<void> DepartureGate::on_departure(net::PeerIndex peer, std::uint64_t cutoff) {
        settled.emplace_back(peer, cutoff);
        return eawr::core::Result<void>::success();
    }

void expect(bool value, const std::string& message) {
    if (!value) local_checker.check_before_print(value, message, std::cerr, "FAIL: ");
}

Fixed units(std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); }

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

std::string agreement(std::uint32_t peers, std::uint64_t delay, std::size_t workers, const std::string& fault,
    std::optional<eawr::testing::WanProfile> wan) {
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
}
