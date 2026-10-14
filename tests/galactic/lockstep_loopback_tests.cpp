// gc_lockstep_loopback (M3 foundation FT-06, ADR-015): the lockstep loopback gate over the
// committed tactical replay corpus with two and eight peers, and negative cases that inject
// a divergence and check the gate names the right tick and peer. Arguments: the directories that
// hold the committed *.eawr-replay files.

#include "../support/lockstep_loopback.hpp"

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace lockstep = eawr::testing::lockstep;
namespace tactical = eawr::sim::tactical;
using Peer = lockstep::Peer<lockstep::TacticalAdapter>;
using Executors = std::vector<const eawr::sim::PartitionExecutor*>;

int failures = 0;

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
    configs[0].label = "2 peers D=3 N=30";
    configs[1].label = "2 peers D=2 N=30 held";
    configs[1].options.input_delay = 2;
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
            if (!report.agreed()) continue;
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

// Pending commands are canonical session state, as in the tactical session. Their
// arrival timing must never enter the hash of a completed simulation tick.
struct PendingAdapter {
    struct Command {
        std::uint64_t tick{}, issuer{}, sequence{}, value{};
        friend bool operator==(const Command&, const Command&) = default;
    };
    struct Session {
        std::uint64_t completed{}, value{};
        std::vector<Command> pending, submitted;
    };

    [[nodiscard]] static std::uint64_t tick(const Command& command) { return command.tick; }
    [[nodiscard]] static std::uint64_t issuer(const Command& command) { return command.issuer; }
    [[nodiscard]] static std::uint64_t sequence(const Command& command) { return command.sequence; }
    [[nodiscard]] static auto key(const Command& command) {
        return std::tuple{command.tick, command.issuer, command.sequence};
    }
    [[nodiscard]] static eawr::core::Result<void> submit(Session& session, const Command& command) {
        session.pending.push_back(command);
        session.submitted.push_back(command);
        return eawr::core::Result<void>::success();
    }
    [[nodiscard]] static eawr::core::Result<std::string> step(Session& session,
        const eawr::sim::PartitionExecutor&) {
        std::sort(session.pending.begin(), session.pending.end(), [](const Command& x, const Command& y) {
            return key(x) < key(y);
        });
        for (const auto& command : session.pending) {
            if (command.tick == session.completed) session.value = session.value * 17 + command.value;
        }
        std::erase_if(session.pending, [&](const Command& command) { return command.tick == session.completed; });
        ++session.completed;
        return eawr::core::Result<std::string>::success(state_hash(session));
    }
    [[nodiscard]] static std::uint64_t completed_tick(const Session& session) { return session.completed; }
    [[nodiscard]] static std::string state_hash(const Session& session) {
        auto pending = session.pending;
        std::sort(pending.begin(), pending.end(), [](const Command& x, const Command& y) { return key(x) < key(y); });
        auto hash = std::to_string(session.completed) + ":" + std::to_string(session.value);
        for (const auto& command : pending) {
            hash += ":" + std::to_string(command.tick) + "/" + std::to_string(command.issuer)
                + "/" + std::to_string(command.sequence) + "/" + std::to_string(command.value);
        }
        return hash;
    }
    [[nodiscard]] static std::vector<std::uint8_t> encoding(const Session& session) {
        const auto hash = state_hash(session);
        return {hash.begin(), hash.end()};
    }
    [[nodiscard]] static eawr::core::Result<std::vector<std::uint8_t>> input_bytes(const Session&,
        const std::span<const Command> batch) {
        std::vector<std::uint8_t> bytes;
        for (const auto& command : batch)
            for (const auto value : {command.tick, command.issuer, command.sequence, command.value})
                for (unsigned byte = 0; byte < 8; ++byte)
                    bytes.push_back(static_cast<std::uint8_t>(value >> (8 * byte)));
        return eawr::core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
    }
};

void pending_commands_ignore_delivery_timing(const eawr::sim::PartitionExecutor& executor) {
    // Interleaved issuer ownership and multiple sequences make sender order insufficient.
    const std::vector<PendingAdapter::Command> commands{
        {3, 1, 0, 1}, {3, 1, 1, 2}, {3, 2, 0, 3}, {3, 3, 0, 4},
        {5, 1, 2, 5}, {5, 2, 1, 6}, {8, 3, 1, 7}};
    auto reference = PendingAdapter::Session{};
    for (std::uint64_t tick = 0; tick < 12; ++tick) {
        for (const auto& command : commands) {
            if (command.tick == tick) static_cast<void>(PendingAdapter::submit(reference, command));
        }
        static_cast<void>(PendingAdapter::step(reference, executor));
    }
    for (const std::uint64_t every : {1ULL, 7ULL}) {
        for (const std::size_t count : {std::size_t{2}, lockstep::max_peers}) {
            for (const std::uint64_t delay : {0ULL, 3ULL, 5ULL}) {
                for (const bool held : {false, true}) {
                    lockstep::Options options;
                    options.final_tick = 12;
                    options.compare_every = every;
                    options.input_delay = delay;
                    if (held) {
                        options.hold_every = 3;
                        options.hold_phase = 1;
                        options.hold_rounds = 4;
                    }
                    auto split = lockstep::split_by_issuer<PendingAdapter>(commands, count);
                    std::vector<lockstep::Peer<PendingAdapter>> peers(count);
                    std::vector<lockstep::Peer<PendingAdapter>*> pointers;
                    bool complete_batches_only = true;
                    for (std::size_t index = 0; index < count; ++index) {
                        peers[index].executor = &executor;
                        peers[index].commands = std::move(split[index]);
                        peers[index].before_step = [&](PendingAdapter::Session& session, const std::uint64_t tick) {
                            const auto expected = static_cast<std::size_t>(std::count_if(commands.begin(), commands.end(),
                                [tick](const auto& command) { return command.tick == tick; }));
                            complete_batches_only = complete_batches_only && session.pending.size() == expected
                                && std::all_of(session.pending.begin(), session.pending.end(),
                                    [tick](const auto& command) { return command.tick == tick; });
                        };
                        pointers.push_back(&peers[index]);
                    }
                    const auto report = lockstep::run(pointers, options);
                    const auto where = "pending N=" + std::to_string(every) + " peers=" + std::to_string(count)
                        + " D=" + std::to_string(delay) + " held=" + std::to_string(held);
                    std::cout << where << ": " << report.describe() << '\n';
                    expect(report.agreed(), where + ": delivery timing produced a false divergence");
                    expect(complete_batches_only, where + ": commands installed outside a complete frontier");
                    if (!report.agreed()) continue;
                    expect(report.final_hash == PendingAdapter::state_hash(reference), where + ": final state");
                    for (const auto& peer : peers) {
                        expect(peer.session.submitted == commands, where + ": submission must follow issuer/sequence order");
                        expect(peer.session.pending.empty(), where + ": commands left pending");
                    }
                    if (held) expect(report.total_stalls() > 0, where + ": delivery was not delayed");
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

} // namespace

int main(int argc, char** argv) {
    std::vector<std::filesystem::path> directories;
    for (int index = 1; index < argc; ++index) directories.emplace_back(argv[index]);
    if (directories.empty()) {
        std::cerr << "usage: gc_lockstep_loopback_tests <replay fixture directory>...\n";
        return 2;
    }
    const auto corpus = load_corpus(directories);
    // The committed tactical corpus (19 replays, two of them format 1) must not silently shrink.
    expect(corpus.size() >= 17, "the tactical replay corpus has " + std::to_string(corpus.size()) + " replays");
    const eawr::sim::InlineExecutor one;
    const eawr::platform::ThreadWorkerAdapter two(2), four(4), eight(8);
    const Executors executors{&one, &two, &four, &eight};
    corpus_gate(corpus, executors);
    transient_state_divergence(one);
    pending_commands_ignore_delivery_timing(one);
    injected_state_divergence(corpus, executors, 2, 1);
    injected_state_divergence(corpus, executors, lockstep::max_peers, 5);
    dropped_command_divergence(corpus, executors);
    starved_peer_is_reported(corpus, executors);
    peer_count_limits(corpus, executors);
    if (failures != 0) {
        std::cerr << failures << " lockstep loopback check(s) failed\n";
        return 1;
    }
    std::cout << "gc_lockstep_loopback: ok\n";
    return 0;
}
