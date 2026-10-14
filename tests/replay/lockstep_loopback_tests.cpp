// Tactical lockstep loopback (shared M3 foundation FT-06, ADR-015): the gate over the
// committed tactical replay corpus with two and eight peers, and negative cases that inject
// a divergence and check the gate names the right tick and peer. Arguments: the directories that
// hold the committed *.eawr-replay files.

#include "../support/lockstep_loopback.hpp"

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/seat.hpp"
#include "eawr/script/authoritative/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace lockstep = eawr::testing::lockstep;
namespace tactical = eawr::sim::tactical;
using Peer = lockstep::Peer<lockstep::TacticalAdapter>;
using Executors = std::vector<const eawr::sim::PartitionExecutor*>;

int failures = 0;
std::filesystem::path report_directory = "out/lockstep-loopback";

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct CorpusReplay {
    std::string name;
    tactical::TacticalReplay replay;
};

// Every committed tactical replay (format 2 and later) except the deliberately malformed ones.
// Format-1 replays carry their whole command list in the world and have no live submission path.
[[nodiscard]] std::vector<CorpusReplay> load_corpus(const std::vector<std::filesystem::path>& directories) {
    std::vector<std::filesystem::path> paths;
    for (const auto& directory : directories) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == ".eawr-replay"
                && entry.path().filename().string().find("mutated") == std::string::npos) {
                paths.push_back(entry.path());
            }
        }
    }
    std::sort(paths.begin(), paths.end());
    std::vector<CorpusReplay> corpus;
    for (const auto& path : paths) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const auto version = tactical::peek_replay_format_version(bytes);
        if (!version || *version < tactical::replay_format_version) {
            std::cout << "skip " << path.filename().string() << " (format 1)\n";
            continue;
        }
        auto replay = tactical::parse_replay(bytes, path.filename().string());
        if (!replay) {
            expect(false, path.filename().string() + ": " + replay.error().message);
            continue;
        }
        corpus.push_back({path.filename().string(), std::move(replay).value()});
    }
    return corpus;
}

// Peer 0 on one inline worker; peers 1, 2 and 3 on 2, 4 and 8 pool workers, and any further peer
// inline (the pool's cost on these small sessions would dominate the gate's time). Every peer but
// peer 0 has its storage scrambled at the start and at every exchange interval. Each drives the players
// the issuer split deals it.
class Peers {
public:
    Peers(const tactical::TacticalReplay& replay, const std::size_t count, const lockstep::Options& options,
        const Executors& executors) {
        auto split = lockstep::split_by_issuer<lockstep::TacticalAdapter>(replay.commands, count);
        const auto every = std::max<std::uint64_t>(options.compare_every, 1);
        for (std::size_t index = 0; index < count; ++index) {
            auto session = tactical::TacticalSession::create(replay.setup);
            if (!session) {
                expect(false, "create: " + session.error().message);
                return;
            }
            peers_.push_back(Peer{std::move(session).value(), index < executors.size() ? executors[index] : executors.front(),
                std::move(split[index]), {}, {}});
            if (index != 0) {
                peers_.back().before_step = [every](tactical::TacticalSession& live, const std::uint64_t tick) {
                    if (tick % every == 0) live.scramble_storage_for_testing();
                };
            }
        }
        ok_ = true;
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] Peer& operator[](const std::size_t index) { return peers_[index]; }
    [[nodiscard]] std::vector<Peer*> pointers() {
        std::vector<Peer*> result;
        for (auto& peer : peers_) result.push_back(&peer);
        return result;
    }

private:
    std::vector<Peer> peers_;
    bool ok_{};
};

void persist_tactical_report(const std::string& label, const tactical::TacticalSession& reference,
    const tactical::TacticalSession& peer, const lockstep::Report& report) {
    const auto directory = report_directory / label;
    std::filesystem::create_directories(directory);
    const auto write = [&](const char* name, const std::vector<std::uint8_t>& bytes) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error("cannot persist loopback diagnostic artifact");
    };
    auto a = tactical::write_replay(reference.record()), b = tactical::write_replay(peer.record());
    if (!a || !b) throw std::runtime_error("cannot encode divergent peer recordings");
    write("peer-0.eawr-replay", a.value()); write("peer-other.eawr-replay", b.value());
    if (report.divergence) {
        write("peer-0.input", report.divergence->input_reference);
        write("peer-other.input", report.divergence->input_peer);
        write("peer-0.state", report.divergence->encoding_reference);
        write("peer-other.state", report.divergence->encoding_peer);
    }
    std::ofstream text(directory / "divergence.txt"); text << report.describe() << '\n';
    if (!text) throw std::runtime_error("cannot persist loopback diagnostic description");
    std::cout << "loopback artifacts: " << directory.generic_string() << '\n';
}

[[nodiscard]] std::string reference_final_hash(const tactical::TacticalReplay& replay,
    const eawr::sim::PartitionExecutor& executor) {
    auto session = tactical::TacticalSession::from_replay(replay);
    if (!session) return "from_replay: " + session.error().message;
    while (session.value().completed_tick() < replay.final_tick_count) {
        const auto tick = session.value().step(executor);
        if (!tick) return "step: " + tick.error().message;
    }
    return session.value().state_sha256();
}

struct Config {
    std::string_view label;
    std::size_t peers{2};
    lockstep::Options options;
};

// The corpus under three transports: two peers at the default delay, two peers with a short delay
// and held batches, and eight peers (the coop seat limit; most of them drive fewer players than
// the others or none) with a long delay, held batches and an exchange every tick.
void corpus_gate(const std::vector<CorpusReplay>& corpus, const Executors& executors) {
    std::vector<Config> configs(3);
    configs[0].label = "2 peers D=2 N=1";
    configs[0].options.input_delay = 2;
    configs[0].options.compare_every = 1;
    configs[1].label = "2 peers D=5 N=1 held";
    configs[1].options.compare_every = 1;
    configs[1].options.input_delay = 5;
    configs[1].options.hold_every = 7;
    configs[1].options.hold_phase = 3;
    configs[1].options.hold_rounds = 4;
    configs[2].label = "8 peers D=5 N=1 held";
    configs[2].peers = lockstep::max_peers;
    configs[2].options.input_delay = 5;
    configs[2].options.compare_every = 1;
    configs[2].options.hold_every = 11;
    configs[2].options.hold_rounds = 9;
    configs[2].options.keep_encodings = false;
    for (const auto& config : configs) {
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t stalls = 0, remote = 0, ticks = 0;
        for (const auto& [name, replay] : corpus) {
            auto options = config.options;
            options.final_tick = replay.final_tick_count;
            Peers peers(replay, config.peers, options, executors);
            if (!peers.ok()) continue;
            const auto report = lockstep::run(peers.pointers(), options);
            const auto where = std::string(config.label) + " " + name;
            expect(report.agreed(), where + ": " + report.describe());
            if (!report.agreed()) {
                const auto peer = report.divergence ? report.divergence->peer : 1;
                persist_tactical_report("unexpected-" + name, peers[0].session, peers[peer].session, report);
                continue;
            }
            expect(report.completed_ticks == replay.final_tick_count, where + ": stopped early");
            for (std::size_t index = 0; index < config.peers; ++index) {
                // Every peer executed exactly the recorded stream: live equals replay.
                expect(peers[index].session.completed_tick() == replay.final_tick_count,
                    where + ": peer " + std::to_string(index) + " did not reach the final tick");
                expect(peers[index].session.record() == replay,
                    where + ": peer " + std::to_string(index) + "'s record differs from the replay");
            }
            const auto expected_exchanges = 1 + (replay.final_tick_count + options.compare_every - 1)
                / options.compare_every;
            expect(report.exchanges == expected_exchanges, where + ": exchange count");
            if (&config == &configs.front()) {
                // Arrival timing never changes state: equal to the whole stream submitted up front.
                expect(report.final_hash == reference_final_hash(replay, *executors.front()),
                    where + ": final hash differs from the up-front replay");
            }
            stalls += report.total_stalls();
            remote += report.remote_commands;
            ticks += report.completed_ticks;
        }
        std::cout << config.label << ": " << corpus.size() << " replays, " << ticks << " ticks, " << remote
                  << " remote commands, " << stalls << " stalls, "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
                  << " ms\n";
        expect(remote > 0, std::string(config.label) + ": no command crossed the transport");
        if (config.options.hold_every != 0) {
            expect(stalls > 0, std::string(config.label) + ": held batches never made a peer wait");
        }
    }
}

// A minimal session whose fault lasts one tick, then reconverges. This exercises the
// session-agnostic harness without relying on a tactical mutation remaining permanent.
struct TransientAdapter {
    struct Session {
        std::uint64_t completed{};
        bool different{};
    };
    struct Command {
        std::uint64_t tick{}, issuer{};
    };

    [[nodiscard]] static std::uint64_t tick(const Command& command) { return command.tick; }
    [[nodiscard]] static std::uint64_t issuer(const Command& command) { return command.issuer; }
    [[nodiscard]] static std::uint64_t sequence(const Command&) { return 0; }
    [[nodiscard]] static eawr::core::Result<void> submit(Session&, const Command&) {
        return eawr::core::Result<void>::success();
    }
    [[nodiscard]] static eawr::core::Result<std::string> step(Session& session,
        const eawr::sim::PartitionExecutor&) {
        ++session.completed;
        return eawr::core::Result<std::string>::success(state_hash(session));
    }
    [[nodiscard]] static std::uint64_t completed_tick(const Session& session) { return session.completed; }
    [[nodiscard]] static std::string state_hash(const Session& session) {
        return std::to_string(session.completed) + (session.different ? ":different" : ":equal");
    }
    [[nodiscard]] static std::vector<std::uint8_t> encoding(const Session& session) {
        return {static_cast<std::uint8_t>(session.completed), static_cast<std::uint8_t>(session.different)};
    }
    [[nodiscard]] static eawr::core::Result<std::vector<std::uint8_t>> input_bytes(const Session&,
        const std::span<const Command> batch) {
        std::vector<std::uint8_t> bytes;
        for (const auto& command : batch) for (const auto value : {command.tick, command.issuer})
            for (unsigned byte = 0; byte < 8; ++byte) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * byte)));
        return eawr::core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
    }
};

void transient_state_divergence(const eawr::sim::PartitionExecutor& executor) {
    using TransientPeer = lockstep::Peer<TransientAdapter>;
    // N=30 reproduces the missed tick-48 fault at tick 60; N=7 reconverges exactly at
    // the next exchange; N=100 compares only the final, partial window; N=1 is the control.
    for (const std::uint64_t every : {30ULL, 7ULL, 100ULL, 1ULL}) {
        for (const std::size_t count : {std::size_t{2}, lockstep::max_peers}) {
            for (const bool keep_encodings : {false, true}) {
                for (const bool held : {false, true}) {
                    lockstep::Options options;
                    options.compare_every = every;
                    options.final_tick = every == 100 ? 55 : 90;
                    options.keep_encodings = keep_encodings;
                    if (held) {
                        options.input_delay = 0;
                        options.hold_every = 7;
                        options.hold_rounds = 4;
                    }
                    std::vector<TransientPeer> peers(count);
                    std::vector<TransientPeer*> pointers;
                    for (auto& peer : peers) {
                        peer.executor = &executor;
                        pointers.push_back(&peer);
                    }
                    const std::size_t target = count == 2 ? 1 : 5;
                    peers[target].before_step = [](TransientAdapter::Session& session, const std::uint64_t tick) {
                        session.different = tick == 47 || tick == 52;
                    };
                    if (count > 2) {
                        // A lower-index peer differs later: choose the earliest tick, then the peer.
                        peers[1].before_step = [](TransientAdapter::Session& session, const std::uint64_t tick) {
                            session.different = tick == 49;
                        };
                    }
                    const auto report = lockstep::run(pointers, options);
                    const auto where = "transient N=" + std::to_string(every) + " peers=" + std::to_string(count)
                        + " encodings=" + std::to_string(keep_encodings) + " held=" + std::to_string(held);
                    std::cout << where << ": " << report.describe() << '\n';
                    expect(!report.error, where + ": unexpected loopback error");
                    expect(!report.agreed() && report.divergence.has_value(), where + ": transient fault was erased");
                    if (!report.divergence) continue;
                    const auto& divergence = *report.divergence;
                    const auto detected = std::min<std::uint64_t>(((48 + every - 1) / every) * every, options.final_tick);
                    expect(divergence.completed_tick == 48, where + ": wrong first divergent tick");
                    expect(divergence.detected_at == detected, where + ": wrong exchange point");
                    expect(divergence.peer == target, where + ": wrong first divergent peer");
                    expect(divergence.hash_reference == "48:equal" && divergence.hash_peer == "48:different",
                        where + ": lost the divergent hashes");
                    expect(report.exchanges == 1 + (48 + every - 1) / every, where + ": wrong exchange count");
                    if (keep_encodings) {
                        const bool reconverged = detected != 48;
                        expect(!divergence.encoding_reference.empty()
                                && (divergence.encoding_reference == divergence.encoding_peer) == reconverged,
                            where + ": exchange encodings must reflect reconvergence");
                        expect(divergence.first_differing_byte == (reconverged ? 2U : 1U),
                            where + ": wrong encoding difference offset");
                        if (reconverged) {
                            expect(report.describe().find("encodings at tick " + std::to_string(detected) + " agree")
                                    != std::string::npos,
                                where + ": diagnostics must say the exchange encodings agree");
                        }
                    } else {
                        expect(divergence.encoding_reference.empty() && divergence.encoding_peer.empty(),
                            where + ": encodings retained when disabled");
                    }
                }
            }
        }
    }
}

// A state change on one peer alone, before it steps from completed tick 47, must be named as
// completed tick 48 and that peer at the exchange after tick 60, with both encodings.
void injected_state_divergence(const std::vector<CorpusReplay>& corpus, const Executors& executors,
    const std::size_t count, const std::size_t target) {
    constexpr std::uint64_t injected = 47;
    const CorpusReplay* chosen = nullptr;
    for (const auto& entry : corpus) {
        if (entry.replay.final_tick_count >= 90 && !entry.replay.setup.units.empty()) {
            chosen = &entry;
            break;
        }
    }
    expect(chosen != nullptr, "no corpus replay of 90 ticks with units for the divergence case");
    if (chosen == nullptr) return;
    const auto where = chosen->name + " (" + std::to_string(count) + " peers, peer " + std::to_string(target) + ")";
    lockstep::Options options;
    options.final_tick = chosen->replay.final_tick_count;
    Peers peers(chosen->replay, count, options, executors);
    if (!peers.ok()) return;
    bool removed = false;
    auto scramble = peers[target].before_step;
    peers[target].before_step = [&removed, scramble](tactical::TacticalSession& session, const std::uint64_t tick) {
        if (scramble) scramble(session, tick);
        if (tick != injected) return;
        const auto units = session.units();
        for (auto unit = units.rbegin(); unit != units.rend() && !removed; ++unit) {
            removed = static_cast<bool>(session.stage_remove(unit->entity_id));
        }
    };
    const auto report = lockstep::run(peers.pointers(), options);
    expect(removed, where + ": the injected removal did not happen");
    expect(!report.error, where + ": " + report.describe());
    expect(report.divergence.has_value(), where + ": the injected divergence was not caught");
    if (!report.divergence) return;
    persist_tactical_report("state-fault-" + std::to_string(count), peers[0].session, peers[target].session, report);
    const auto& divergence = *report.divergence;
    std::cout << "injected on " << where << ": " << report.describe() << '\n';
    expect(divergence.completed_tick == injected + 1, where + ": wrong divergent tick");
    expect(divergence.detected_at == 60, where + ": wrong exchange point");
    expect(divergence.peer == target, where + ": wrong peer");
    expect(divergence.hash_reference != divergence.hash_peer && !divergence.hash_reference.empty(), where + ": hashes");
    const auto& x = divergence.encoding_reference;
    const auto& y = divergence.encoding_peer;
    expect(!x.empty() && !y.empty() && x != y, where + ": encodings");
    const auto shared = std::min(x.size(), y.size());
    const auto at = std::min(divergence.first_differing_byte, shared);
    expect(divergence.first_differing_byte <= shared && (at == shared || x[at] != y[at])
            && std::equal(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(at), y.begin()),
        where + ": first differing byte");
}

// The transport losing one move order on its way from peer 0 to peer 1 is an input divergence:
// caught no earlier than the tick after the lost order executes, and no later than the next exchange.
void dropped_command_divergence(const std::vector<CorpusReplay>& corpus, const Executors& executors) {
    bool tested = false;
    for (const auto& [name, replay] : corpus) {
        lockstep::Options options;
        options.final_tick = replay.final_tick_count;
        Peers peers(replay, 2, options, executors);
        if (!peers.ok()) continue;
        std::optional<std::uint64_t> dropped;
        peers[1].on_receive = [&dropped](std::vector<tactical::PlayerCommand>& batch, std::uint64_t, std::size_t) {
            if (dropped) return;
            const auto move = std::find_if(batch.begin(), batch.end(), [](const tactical::PlayerCommand& command) {
                return std::holds_alternative<tactical::MovePayload>(command.payload);
            });
            if (move == batch.end()) return;
            dropped = move->key.tick;
            batch.erase(move);
        };
        const auto report = lockstep::run(peers.pointers(), options);
        if (!dropped) continue;
        tested = true;
        std::cout << "dropped move at tick " << *dropped << " in " << name << ": " << report.describe() << '\n';
        expect(!report.error, name + ": " + report.describe());
        expect(report.divergence.has_value(), name + ": the lost command was not caught");
        if (report.divergence) {
            persist_tactical_report("dropped-input", peers[0].session, peers[1].session, report);
            expect(report.divergence->completed_tick > *dropped
                    && report.divergence->completed_tick <= report.divergence->detected_at
                    && report.divergence->peer == 1,
                name + ": divergence outside the lost command's window");
        }
        break;
    }
    expect(tested, "no corpus replay sends a move order across the transport");
}

// A peer whose stream never arrives is a reported stall, not a hang.
void starved_peer_is_reported(const std::vector<CorpusReplay>& corpus, const Executors& executors) {
    for (const auto& [name, replay] : corpus) {
        if (replay.final_tick_count < 10) continue;
        lockstep::Options options;
        options.final_tick = replay.final_tick_count;
        options.hold_every = 1;
        options.hold_rounds = std::uint64_t{1} << 40U;
        options.max_wait_rounds = 50;
        Peers peers(replay, 2, options, executors);
        if (!peers.ok()) return;
        const auto report = lockstep::run(peers.pointers(), options);
        expect(report.error.has_value() && !report.divergence, name + ": a starved peer must stall");
        expect(report.completed_ticks == 0 && report.rounds <= 60, name + ": starved run");
        return;
    }
}

// The harness refuses fewer than two or more than eight peers.
void peer_count_limits(const std::vector<CorpusReplay>& corpus, const Executors& executors) {
    if (corpus.empty()) return;
    for (const std::size_t count : {std::size_t{1}, lockstep::max_peers + 1}) {
        Peers peers(corpus.front().replay, count, {}, executors);
        if (!peers.ok()) continue;
        const auto report = lockstep::run(peers.pointers(), {});
        expect(report.error.has_value(), std::to_string(count) + " peers must be refused");
    }
}

void sealed_faults(const eawr::sim::PartitionExecutor& executor) {
    using TestPeer = lockstep::Peer<TransientAdapter>;
    for (const auto delay : {2ULL, 5ULL}) {
        lockstep::Options options;
        options.input_delay = delay;
        options.compare_every = 1;
        options.final_tick = 12;
        options.max_wait_rounds = 20;
        options.drop_batch = [](std::size_t receiver, std::size_t sender, std::uint64_t tick) {
            return receiver == 1 && sender == 0 && tick == 4;
        };
        TestPeer a{{}, &executor, {}, {}, {}}, b{{}, &executor, {}, {}, {}};
        const auto missing = lockstep::run<TransientAdapter>({&a, &b}, options);
        expect(missing.error && !missing.divergence && b.session.completed == 4,
            "a dropped explicit empty batch holds its receiver before that tick, never steps it");
        options.drop_batch = {};
        a = TestPeer{{}, &executor, {{4, 1}}, {}, {}};
        b = TestPeer{{}, &executor, {}, {}, {}};
        b.on_receive = [](std::vector<TransientAdapter::Command>& batch, std::uint64_t, std::size_t) {
            if (!batch.empty()) ++batch.front().tick;
        };
        const auto late = lockstep::run<TransientAdapter>({&a, &b}, options);
        expect(late.error && late.error->message.find("sealed command lateness at tick 4") != std::string::npos,
            "sealed command delivered with another tick is refused, never restamped");
        a = TestPeer{{}, &executor, {{4, 1}}, {}, {}};
        b = TestPeer{{}, &executor, {}, {}, {}};
        b.on_receive = [](std::vector<TransientAdapter::Command>& batch, std::uint64_t, std::size_t) { batch.clear(); };
        options.compare_every = 10;
        const auto no_op = lockstep::run<TransientAdapter>({&a, &b}, options);
        expect(no_op.divergence && no_op.divergence->completed_tick == 5
            && no_op.divergence->channel == "input history" && a.session.completed == b.session.completed,
            "input history catches a lost no-op despite equal world state at the exchange");
    }
}

// Same-faction seats share a child stream; the router operates only after tick completeness.
struct SeatAdapter {
    struct Session {
        tactical::TacticalSession world;
        std::vector<tactical::SeatCommand> pending;
        std::uint64_t faction_sequence{};
    };
    using Command = tactical::SeatCommand;
    static std::uint64_t tick(const Command& command) { return command.key.tick; }
    static std::uint64_t issuer(const Command& command) { return command.key.seat.value; }
    static std::uint64_t sequence(const Command& command) { return command.key.sequence; }
    static eawr::core::Result<void> submit(Session& session, const Command& command) {
        auto admitted = tactical::admit_human_command({command.key.seat, 1, true, false}, command);
        if (!admitted) return eawr::core::Result<void>::failure(admitted.error());
        session.pending.push_back(command);
        return eawr::core::Result<void>::success();
    }
    static eawr::core::Result<std::string> step(Session& session, const eawr::sim::PartitionExecutor& executor) {
        for (const auto& command : session.pending) {
            auto result = session.world.submit_human({command.key.seat, 1, true, false}, command, session.faction_sequence++);
            if (!result) return eawr::core::Result<std::string>::failure(result.error());
        }
        session.pending.clear();
        return lockstep::TacticalAdapter::step(session.world, executor);
    }
    static std::uint64_t completed_tick(const Session& session) { return session.world.completed_tick(); }
    static std::string state_hash(const Session& session) { return session.world.state_sha256(); }
    static std::vector<std::uint8_t> encoding(const Session& session) { return session.world.canonical_state_bytes(); }
    static eawr::core::Result<std::vector<std::uint8_t>> input_bytes(const Session& session, std::span<const Command> batch) {
        auto replay = session.world.record_through_next_tick();
        replay.commands.clear();
        std::vector<std::uint8_t> provenance;
        auto sequence = session.faction_sequence;
        for (const auto& command : batch) {
            replay.commands.push_back({{command.key.tick, command.player, sequence++}, command.units, command.payload});
            for (const auto value : {static_cast<std::uint64_t>(command.key.seat.value), command.key.sequence})
                for (unsigned byte = 0; byte < 8; ++byte) provenance.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
        }
        auto bytes = tactical::write_replay(replay);
        if (!bytes) return bytes;
        bytes.value().insert(bytes.value().end(), provenance.begin(), provenance.end());
        return bytes;
    }
};

struct LegacyAdapter {
    struct Session {
        eawr::sim::World world;
        eawr::sim::Replay source;
        std::vector<eawr::sim::Command> admitted;
    };
    using Command = eawr::sim::Command;
    static std::uint64_t tick(const Command& command) { return command.key.tick; }
    static std::uint64_t issuer(const Command& command) { return command.key.player_id; }
    static std::uint64_t sequence(const Command& command) { return command.key.sequence; }
    static eawr::core::Result<void> submit(Session& session, const Command& command) {
        session.admitted.push_back(command);
        return eawr::core::Result<void>::success();
    }
    static std::uint64_t completed_tick(const Session& session) { return session.world.completed_tick(); }
    static std::string state_hash(const Session& session) { return session.world.state_sha256(); }
    static std::vector<std::uint8_t> encoding(const Session& session) {
        const auto hash = state_hash(session);
        return {hash.begin(), hash.end()};
    }
    static eawr::core::Result<std::vector<std::uint8_t>> input_bytes(const Session& session, std::span<const Command> batch) {
        auto replay = session.source;
        replay.commands.assign(batch.begin(), batch.end());
        replay.final_tick_count = completed_tick(session) + 1;
        return eawr::sim::write_replay(replay);
    }
    static eawr::core::Result<std::string> step(Session& session, const eawr::sim::PartitionExecutor& executor) {
        std::vector<Command> expected;
        for (const auto& command : session.source.commands) if (tick(command) == completed_tick(session)) expected.push_back(command);
        if (expected != session.admitted) {
            eawr::core::Diagnostic error; error.code = "EAWR-TEST-LOCKSTEP";
            error.message = "legacy recorded input differs from complete sealed batch";
            return eawr::core::Result<std::string>::failure(std::move(error));
        }
        session.admitted.clear();
        auto result = session.world.step(executor);
        if (!result) return eawr::core::Result<std::string>::failure(result.error());
        return eawr::core::Result<std::string>::success(result.value().state_sha256);
    }
};

std::size_t legacy_corpus_gate(const std::vector<std::filesystem::path>& directories, const Executors& executors) {
    std::size_t count = 0;
    for (const auto& directory : directories) for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".eawr-replay" || entry.path().filename().string().find("mutated") != std::string::npos) continue;
        std::ifstream input(entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (tactical::peek_replay_format_version(bytes) != eawr::sim::replay_format_version) continue;
        auto replay = eawr::sim::parse_replay(bytes);
        expect(static_cast<bool>(replay), "legacy corpus parses");
        if (!replay) continue;
        ++count;
        for (const auto delay : {2ULL, 5ULL}) {
            auto a = eawr::sim::World::create(replay.value()), b = eawr::sim::World::create(replay.value());
            expect(a && b, "legacy worlds create");
            if (!a || !b) continue;
            auto split = lockstep::split_by_issuer<LegacyAdapter>(replay.value().commands, 2);
            lockstep::Peer<LegacyAdapter> left{{std::move(a).value(), replay.value(), {}}, executors.front(), std::move(split[0]), {}, {}};
            lockstep::Peer<LegacyAdapter> right{{std::move(b).value(), replay.value(), {}}, executors.back(), std::move(split[1]), {}, {}};
            lockstep::Options options;
            options.final_tick = replay.value().final_tick_count; options.input_delay = delay; options.compare_every = 1;
            options.hold_every = 3; options.hold_rounds = 4;
            const auto report = lockstep::run<LegacyAdapter>({&left, &right}, options);
            expect(report.agreed(), "legacy recorded-input frontier: " + report.describe());
        }
    }
    return count;
}

void shared_owner_conflicts(const Executors& executors) {
    for (const auto delay : {2ULL, 5ULL}) {
        for (std::size_t workers = 0; workers < executors.size(); ++workers) {
            tactical::TacticalSetup setup;
            setup.players = {{1, 0, 1, tactical::player_flag_commandable}};
            setup.units = {{10, 1, 1}};
            auto left = tactical::TacticalSession::create(setup), right = tactical::TacticalSession::create(setup);
            expect(left && right, "shared owner fixture creates");
            if (!left || !right) continue;
            using TestPeer = lockstep::Peer<SeatAdapter>;
            const auto point = eawr::sim::math::Fixed::from_integer(100).value();
            TestPeer a{{std::move(left).value(), {}, 0}, executors.front(),
                {{{0, {1}, 0}, 1, {10}, tactical::MovePayload{{point, {}, {}}}}}, {}, {}};
            TestPeer b{{std::move(right).value(), {}, 0}, executors[workers],
                {{{0, {2}, 0}, 1, {10}, tactical::StopPayload{}}}, {}, {}};
            lockstep::Options options;
            options.final_tick = 3; options.input_delay = delay; options.compare_every = 1;
            options.hold_every = 2; options.hold_rounds = 4;
            const auto report = lockstep::run<SeatAdapter>({&a, &b}, options);
            expect(report.agreed(), "shared faction conflicts agree with delay/worker changes: " + report.describe());
            const auto record = a.session.world.record();
            expect(record.commands.size() == 2 && record.commands[0].key.sequence == 0
                && record.commands[1].key.sequence == 1 && a.session.world.units().front().order.kind == tactical::OrderKind::stop,
                "canonical seat order derives child sequences and the later seat's conflicting order wins");
        }
    }
}

// A real authoritative Lua instance on each peer; world, script and input histories agree
// each tick. An extra authoritative RNG draw can be invisible to the world.
struct ScriptAdapter {
    struct Session {
        tactical::TacticalSession world;
        mutable eawr::script::authoritative::ScriptScheduler scripts;
        bool extra_draw{};
    };
    using Command = tactical::PlayerCommand;
    static std::uint64_t tick(const Command& command) { return command.key.tick; }
    static std::uint64_t issuer(const Command& command) { return command.key.player_id; }
    static std::uint64_t sequence(const Command& command) { return command.key.sequence; }
    static eawr::core::Result<void> submit(Session& session, const Command& command) { return session.world.submit(command); }
    static std::uint64_t completed_tick(const Session& session) { return session.world.completed_tick(); }
    static std::string state_hash(const Session& session) {
        auto script = session.scripts.state_hash();
        if (!script) throw std::runtime_error("script hash error: " + script.error().message);
        return eawr::script::authoritative::authoritative_state_sha256(session.world.completed_tick(),
            session.world.state_sha256(), script.value());
    }
    static std::vector<std::uint8_t> encoding(const Session& session) {
        auto bytes = session.world.canonical_state_bytes();
        auto script = session.scripts.state_hash();
        if (script) bytes.insert(bytes.end(), script.value().begin(), script.value().end());
        return bytes;
    }
    static eawr::core::Result<std::vector<std::uint8_t>> input_bytes(const Session& session, std::span<const Command> batch) {
        return lockstep::TacticalAdapter::input_bytes(session.world, batch);
    }
    static eawr::core::Result<std::string> step(Session& session, const eawr::sim::PartitionExecutor& executor) {
        namespace auth = eawr::script::authoritative;
        auto world = session.world.step(executor);
        if (!world) return eawr::core::Result<std::string>::failure(world.error());
        auth::ScriptEvent event;
        event.key = {session.world.completed_tick(), auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Pulse";
        event.arguments = {auth::Value{session.extra_draw}};
        auto submitted = session.scripts.submit_event(std::move(event));
        if (!submitted) return eawr::core::Result<std::string>::failure(submitted.error());
        auto serviced = session.scripts.service(executor);
        if (!serviced) return eawr::core::Result<std::string>::failure(serviced.error());
        if (!serviced.value().diagnostics.empty()) {
            eawr::core::Diagnostic error; error.code = "EAWR-TEST-LOCKSTEP";
            error.message = serviced.value().diagnostics.front().message;
            return eawr::core::Result<std::string>::failure(std::move(error));
        }
        return eawr::core::Result<std::string>::success(state_hash(session));
    }
};

void script_world_and_rng(const Executors& executors) {
    namespace auth = eawr::script::authoritative;
    using TestPeer = lockstep::Peer<ScriptAdapter>;
    for (const auto seed : {12ULL, 18ULL, 40ULL}) for (const auto count : {std::size_t{2}, lockstep::max_peers}) {
        for (const bool fault : {false, true}) {
            std::vector<TestPeer> peers;
            for (std::size_t index = 0; index < count; ++index) {
                tactical::TacticalSetup setup; setup.seed = seed;
                setup.players = {{1, 0, 1, tactical::player_flag_commandable}};
                auto world = tactical::TacticalSession::create(setup);
                auth::ModuleManifest manifest;
                expect(static_cast<bool>(manifest.add("Loopback.lua",
                    "State = { value = 0 }\nfunction Pulse(extra)\n"
                    "if extra then local discarded = Loopback_Random() end\nState.value = Loopback_Random()\nend\n")),
                    "script fixture manifest adds");
                auth::SessionConfig config; config.seed = seed;
                auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
                expect(world && scripts, "script/world fixture creates");
                if (!world || !scripts) return;
                expect(static_cast<bool>(scripts.value().register_binding("Loopback_Random",
                    [](auth::BindingContext& context, const auth::ValueList&) {
                        return eawr::core::Result<auth::ValueList>::success({auth::Value{context.random().next_unit()}});
                    })), "script fixture uses the authoritative per-instance RNG");
                expect(static_cast<bool>(scripts.value().create_instance(1, "Loopback.lua")), "script fixture instance creates");
                peers.push_back(TestPeer{{std::move(world).value(), std::move(scripts).value(), false},
                    executors[index % executors.size()], {}, {}, {}});
            }
            if (fault) peers[1].before_step = [](ScriptAdapter::Session& session, std::uint64_t tick) {
                session.extra_draw = tick == 7;
            };
            std::vector<TestPeer*> pointers; for (auto& peer : peers) pointers.push_back(&peer);
            lockstep::Options options;
            options.final_tick = 40; options.input_delay = count == 2 ? 2 : 5;
            options.compare_every = fault ? 30 : 1;
            options.hold_every = 7; options.hold_rounds = 3;
            const auto report = lockstep::run(pointers, options);
            if (report.divergence) persist_tactical_report("script-rng-" + std::to_string(seed) + "-" + std::to_string(count),
                peers[0].session.world, peers[1].session.world, report);
            if (!fault) expect(report.agreed(), "full world/script/input history agrees: " + report.describe());
            else expect(report.divergence && !report.error && report.divergence->completed_tick == 8
                && report.divergence->detected_at == 30 && report.divergence->channel == "world/script",
                "an extra authoritative RNG draw is detected at its first tick despite equal world state");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> directories;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--artifacts") {
            if (++index == argc) return 2;
            report_directory = argv[index];
        } else directories.emplace_back(argv[index]);
    }
    if (directories.empty()) {
        std::cerr << "usage: tactical_lockstep_loopback_tests <replay fixture directory>... [--artifacts DIR]\n";
        return 2;
    }
    const auto corpus = load_corpus(directories);
    // The committed tactical corpus (19 replays, two of them format 1) must not silently shrink.
    expect(corpus.size() >= 17, "the tactical replay corpus has " + std::to_string(corpus.size()) + " replays");
    const eawr::sim::InlineExecutor one;
    const eawr::platform::ThreadWorkerAdapter two(2), four(4), eight(8);
    const Executors executors{&one, &two, &four, &eight};
    corpus_gate(corpus, executors);
    expect(corpus.size() + legacy_corpus_gate(directories, executors) == 19, "all 19 valid recorded fixtures qualify");
    transient_state_divergence(one);
    injected_state_divergence(corpus, executors, 2, 1);
    injected_state_divergence(corpus, executors, lockstep::max_peers, 5);
    dropped_command_divergence(corpus, executors);
    starved_peer_is_reported(corpus, executors);
    peer_count_limits(corpus, executors);
    sealed_faults(one);
    shared_owner_conflicts(executors);
    script_world_and_rng(executors);
    if (failures != 0) {
        std::cerr << failures << " lockstep loopback check(s) failed\n";
        return 1;
    }
    std::cout << "tactical_lockstep_loopback: ok\n";
    return 0;
}
