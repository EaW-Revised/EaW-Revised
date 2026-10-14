#pragma once

// Lockstep loopback gate (ADR-015, M3 foundation FT-06): two to eight sessions in one process,
// each fed its own commands plus every other peer's sealed per-tick batches through an
// in-process transport with an input delay, agree on the state hash tick for tick. It is the
// network-free form of lockstep agreement (docs/behaviour/walks/multiplayer.md MP-01, MP-03,
// MP-08, MP-42), sized for the eight human seats of coop galactic conquest:
//
// - a peer seals its batch for tick T, possibly empty, once its own session has completed tick
//   T - D (D = input delay); a sealed batch never changes and reaches every other peer one round
//   later, or later when the transport holds it, and a held batch holds every later one on the
//   same stream (one reliable ordered stream per sender and receiver);
// - a peer buffers inputs outside canonical session state and installs only the complete batch
//   for tick T, in canonical issuer/sequence order, immediately before stepping (no restamping);
// - every peer hashes the setup (the tick-zero barrier) and every completed tick; the hashes are
//   exchanged every N ticks and at the end, and a mismatch names the first completed tick at which
//   a peer differs from peer 0, from the hash rows retained since the last agreement.
//
// The harness is session-agnostic: an adapter (LoopbackAdapter) names the command type, its tick
// and issuer, and how to submit, step, hash and encode a session. The tactical adapter is below;
// the galactic GameSession adapter joins it with the second PR (FT-06b).

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace eawr::testing::lockstep {

inline constexpr std::size_t max_peers = 8;

template <typename A>
concept LoopbackAdapter = requires(typename A::Session& session, const typename A::Session& view,
    const typename A::Command& command, const sim::PartitionExecutor& executor,
    std::span<const typename A::Command> batch) {
    { A::tick(command) } -> std::convertible_to<std::uint64_t>;
    { A::issuer(command) } -> std::convertible_to<std::uint64_t>;
    { A::sequence(command) } -> std::convertible_to<std::uint64_t>;
    { A::submit(session, command) } -> std::same_as<core::Result<void>>;
    // Steps one tick and returns the completed tick's state hash.
    { A::step(session, executor) } -> std::same_as<core::Result<std::string>>;
    { A::completed_tick(view) } -> std::convertible_to<std::uint64_t>;
    { A::state_hash(view) } -> std::same_as<std::string>;
    { A::encoding(view) } -> std::same_as<std::vector<std::uint8_t>>;
    { A::input_bytes(view, batch) } -> std::same_as<core::Result<std::vector<std::uint8_t>>>;
};

struct Options {
    std::uint64_t input_delay{3};   // D: a batch for tick T is sealed after tick T - D completes
    std::uint64_t compare_every{30}; // N: hash exchange interval in completed ticks; the end always
    std::uint64_t final_tick{};      // every peer stops after this many completed ticks
    // Transport faults: sender s's batch for tick T is held for `hold_rounds` extra rounds on every
    // stream when hold_every != 0 and (T + s) % hold_every == hold_phase.
    std::uint64_t hold_every{};
    std::uint64_t hold_phase{};
    std::uint64_t hold_rounds{};
    // A run in which no peer seals, receives or steps for this many rounds in a row fails as a
    // stall (a peer that stopped sending), never a hang.
    std::uint64_t max_wait_rounds{1000};
    // Keep each peer's canonical encoding at every exchange point, so a divergence reports both.
    bool keep_encodings{true};
    // Qualification fault: lose a whole sealed batch, including an explicit empty one.
    // It must hold the receiver at that tick, never turn a missing batch into an empty batch.
    std::function<bool(std::size_t, std::size_t, std::uint64_t)> drop_batch;
};

template <LoopbackAdapter A>
struct Peer {
    typename A::Session session;
    const sim::PartitionExecutor* executor{};
    // This peer's own commands, in canonical order.
    std::vector<typename A::Command> commands;
    // Called with the completed tick before each step: storage scrambling, fault injection.
    std::function<void(typename A::Session&, std::uint64_t)> before_step;
    // Called with every batch this peer receives (its tick and sender), before it is submitted:
    // transport faults.
    std::function<void(std::vector<typename A::Command>&, std::uint64_t, std::size_t)> on_receive;
};

struct Divergence {
    std::uint64_t completed_tick{}; // the first completed tick at which a peer differs (0: the setup)
    std::uint64_t detected_at{};    // the exchange point that found it
    std::size_t peer{};             // the first peer that differs from peer 0 at completed_tick
    std::string hash_reference, hash_peer; // peer 0's and that peer's hashes there
    // Both peers' canonical encodings at the exchange point (empty without keep_encodings), and
    // the first byte at which they differ (their common size when they agree after reconvergence).
    std::vector<std::uint8_t> encoding_reference, encoding_peer;
    std::size_t first_differing_byte{};
    std::string channel{"world/script"};
    std::vector<std::uint8_t> input_reference, input_peer; // canonical batches at the first differing tick
};

struct Report {
    std::optional<Divergence> divergence;
    std::optional<core::Diagnostic> error; // a refused submit, a failed step, a deadlock or a stall
    std::uint64_t completed_ticks{};
    std::uint64_t exchanges{};       // hash comparisons, the tick-zero barrier included
    std::uint64_t rounds{};
    std::uint64_t batches{};         // sealed batches, empty ones included
    std::uint64_t remote_commands{}; // commands delivered from one peer to another
    std::vector<std::uint64_t> stalls; // per peer: rounds it waited for another peer's batch
    std::string final_hash;

    [[nodiscard]] bool agreed() const noexcept { return !divergence && !error; }

    [[nodiscard]] std::uint64_t total_stalls() const noexcept {
        std::uint64_t total = 0;
        for (const auto count : stalls) total += count;
        return total;
    }

    [[nodiscard]] std::string describe() const {
        std::ostringstream text;
        if (error) {
            text << "loopback error " << error->code << ": " << error->message;
        } else if (divergence) {
            text << "lockstep " << divergence->channel << " divergence at completed tick " << divergence->completed_tick
                 << " (found at the exchange after tick " << divergence->detected_at << "): peer 0 "
                 << divergence->hash_reference << ", peer " << divergence->peer << ' ' << divergence->hash_peer;
            if (!divergence->encoding_reference.empty() || !divergence->encoding_peer.empty()) {
                text << "; encodings at tick " << divergence->detected_at;
                if (divergence->encoding_reference == divergence->encoding_peer) {
                    text << " agree";
                } else {
                    text << " differ at byte " << divergence->first_differing_byte << " ("
                         << divergence->encoding_reference.size() << " and " << divergence->encoding_peer.size()
                         << " bytes)";
                }
            }
        } else {
            text << stalls.size() << " peers agreed through tick " << completed_ticks << " (" << exchanges
                 << " exchanges, " << batches << " batches, " << remote_commands << " remote commands, "
                 << total_stalls() << " stalls)";
        }
        return text.str();
    }
};

// Splits a canonical command stream between `peers` peers by issuer: ascending issuers are dealt
// round-robin, so each peer drives whole players (seats in a galactic session) and a peer with no
// issuer only receives.
template <LoopbackAdapter A>
[[nodiscard]] std::vector<std::vector<typename A::Command>> split_by_issuer(
    const std::vector<typename A::Command>& commands, const std::size_t peers) {
    std::set<std::uint64_t> issuers;
    for (const auto& command : commands) issuers.insert(A::issuer(command));
    std::map<std::uint64_t, std::size_t> owner;
    std::size_t next = 0;
    for (const auto issuer : issuers) owner.emplace(issuer, next++ % std::max<std::size_t>(peers, 1));
    std::vector<std::vector<typename A::Command>> split(std::max<std::size_t>(peers, 1));
    for (const auto& command : commands) split[owner.at(A::issuer(command))].push_back(command);
    return split;
}

namespace detail {

inline core::Diagnostic loopback_error(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = "EAWR-TEST-LOCKSTEP";
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    return diagnostic;
}

template <LoopbackAdapter A>
struct Batch {
    std::uint64_t tick{};
    std::uint64_t release_round{};
    std::vector<typename A::Command> commands;
};

template <LoopbackAdapter A>
struct PeerState {
    Peer<A>* peer{};
    std::size_t next_command{};
    std::uint64_t next_seal{}; // the next tick this peer seals a batch for
    // Per sender: the batches in flight to this peer in send order, and the tick below which
    // that sender's batches have all arrived.
    std::vector<std::deque<Batch<A>>> inbox;
    std::vector<std::uint64_t> received_through;
    std::map<std::uint64_t, std::map<std::size_t, std::vector<typename A::Command>>> ready;
    std::map<std::uint64_t, std::string> hashes; // completed tick -> hash, since the last agreement
    std::map<std::uint64_t, std::string> input_hashes;
    std::map<std::uint64_t, std::vector<std::uint8_t>> inputs;
    std::string input_history{"eawr-loopback-input-v1"};
    std::map<std::uint64_t, std::vector<std::uint8_t>> encodings; // at pending exchange points
};

} // namespace detail

// Runs every peer to options.final_tick in deterministic rounds. Each round, each peer in turn
// seals its due batches, takes the other peers' delivered batches, and steps one tick when every
// batch for it is present; then the hashes are exchanged at every exchange point all peers reached.
template <LoopbackAdapter A>
[[nodiscard]] Report run(const std::vector<Peer<A>*>& peers, const Options& options) {
    Report report;
    const std::size_t count = peers.size();
    if (count < 2 || count > max_peers) {
        report.error = detail::loopback_error("a loopback needs 2 to 8 peers, not " + std::to_string(count));
        return report;
    }
    std::vector<detail::PeerState<A>> states(count);
    report.stalls.assign(count, 0);
    const std::uint64_t final_tick = options.final_tick;
    const std::uint64_t every = std::max<std::uint64_t>(options.compare_every, 1);
    const auto is_exchange = [&](const std::uint64_t tick) { return tick % every == 0 || tick == final_tick; };
    const auto reached = [&] {
        std::uint64_t least = final_tick;
        for (const auto* peer : peers) least = std::min<std::uint64_t>(least, A::completed_tick(peer->session));
        return least;
    };

    for (std::size_t index = 0; index < count; ++index) {
        auto& state = states[index];
        state.peer = peers[index];
        state.inbox.resize(count);
        state.received_through.assign(count, 0);
        const auto tick = A::completed_tick(state.peer->session);
        if (tick != 0) {
            report.error = detail::loopback_error("peer " + std::to_string(index) + " starts at completed tick "
                + std::to_string(tick));
            return report;
        }
        state.hashes.emplace(0, A::state_hash(state.peer->session));
        state.input_hashes.emplace(0, state.input_history);
        if (options.keep_encodings) state.encodings.emplace(0, A::encoding(state.peer->session));
    }
    const auto fail = [&](core::Diagnostic error) {
        report.error = std::move(error);
        report.completed_ticks = reached();
        return report;
    };

    std::uint64_t next_exchange = 0;
    std::uint64_t idle_rounds = 0;
    bool finished = false;
    for (std::uint64_t round = 0; !finished; ++round) {
        report.rounds = round + 1;
        bool progress = false;
        for (std::size_t index = 0; index < count; ++index) {
            auto& self = states[index];
            auto& session = self.peer->session;
            const auto completed = A::completed_tick(session);
            // Seal: every tick up to completed + D, each exactly once, empty batches included.
            while (self.next_seal < final_tick && self.next_seal <= completed + options.input_delay) {
                detail::Batch<A> batch;
                batch.tick = self.next_seal;
                auto& own = self.peer->commands;
                while (self.next_command < own.size() && A::tick(own[self.next_command]) <= batch.tick) {
                    const auto& command = own[self.next_command++];
                    if (A::tick(command) < batch.tick) {
                        return fail(detail::loopback_error("peer " + std::to_string(index) + ": a command for tick "
                            + std::to_string(A::tick(command)) + " missed its batch"));
                    }
                    batch.commands.push_back(command);
                }
                const bool held = options.hold_every != 0
                    && (batch.tick + index) % options.hold_every == options.hold_phase;
                batch.release_round = round + 1 + (held ? options.hold_rounds : 0);
                self.ready[batch.tick].emplace(index, batch.commands);
                for (std::size_t receiver = 0; receiver < count; ++receiver) {
                    if (receiver == index) continue;
                    if (options.drop_batch && options.drop_batch(receiver, index, batch.tick)) continue;
                    auto& stream = states[receiver].inbox[index];
                    auto copy = batch;
                    if (!stream.empty()) copy.release_round = std::max(copy.release_round, stream.back().release_round);
                    stream.push_back(std::move(copy));
                }
                ++self.next_seal;
                ++report.batches;
                progress = true;
            }
            // Receive: the delivered prefix of every other peer's ordered stream, in sender order.
            for (std::size_t sender = 0; sender < count; ++sender) {
                auto& stream = self.inbox[sender];
                while (!stream.empty() && stream.front().release_round <= round) {
                    auto batch = std::move(stream.front());
                    stream.pop_front();
                    // Require the next sealed tick, including empties; a gap never advances completeness.
                    if (batch.tick != self.received_through[sender]) {
                        stream.push_front(std::move(batch));
                        break;
                    }
                    if (self.peer->on_receive) self.peer->on_receive(batch.commands, batch.tick, sender);
                    for (const auto& command : batch.commands) {
                        if (A::tick(command) != batch.tick) return fail(detail::loopback_error(
                            "sealed command lateness at tick " + std::to_string(batch.tick)
                            + ": command tick " + std::to_string(A::tick(command))));
                        ++report.remote_commands;
                    }
                    self.ready[batch.tick].emplace(sender, std::move(batch.commands));
                    self.received_through[sender] = batch.tick + 1;
                    progress = true;
                }
            }
            // Step: only with every peer's batch for the next tick.
            if (completed < final_tick) {
                bool complete = self.next_seal > completed;
                for (std::size_t sender = 0; sender < count; ++sender) {
                    if (sender != index && self.received_through[sender] <= completed) complete = false;
                }
                if (complete) {
                    // Pending commands are hashed state. Buffer future/partial inputs outside the session;
                    // install only this complete tick in the same canonical order on every peer.
                    std::vector<typename A::Command> canonical;
                    auto& ready = self.ready.at(completed);
                    for (const auto& [sender, commands] : ready) {
                        static_cast<void>(sender);
                        canonical.insert(canonical.end(), commands.begin(), commands.end());
                    }
                    std::stable_sort(canonical.begin(), canonical.end(), [](const auto& left, const auto& right) {
                        if (A::issuer(left) != A::issuer(right)) return A::issuer(left) < A::issuer(right);
                        return A::sequence(left) < A::sequence(right);
                    });
                    for (const auto& command : canonical) {
                        if (auto submitted = A::submit(session, command); !submitted) return fail(submitted.error());
                    }
                    auto input = A::input_bytes(session, canonical);
                    if (!input) return fail(input.error());
                    self.inputs.emplace(completed + 1, input.value());
                    std::vector<std::uint8_t> history(self.input_history.begin(), self.input_history.end());
                    for (unsigned byte = 0; byte < 8; ++byte)
                        history.push_back(static_cast<std::uint8_t>(completed >> (byte * 8)));
                    history.insert(history.end(), input.value().begin(), input.value().end());
                    self.input_history = core::sha256_hex(history);
                    self.input_hashes.emplace(completed + 1, self.input_history);
                    self.ready.erase(completed);
                    if (self.peer->before_step) self.peer->before_step(session, completed);
                    auto hash = A::step(session, *self.peer->executor);
                    if (!hash) return fail(hash.error());
                    const auto tick = A::completed_tick(session);
                    self.hashes.emplace(tick, std::move(hash).value());
                    if (options.keep_encodings && is_exchange(tick)) self.encodings.emplace(tick, A::encoding(session));
                    progress = true;
                } else {
                    ++report.stalls[index];
                }
            }
        }
        // Exchange: every point all peers reached, in order; a mismatch names the first differing row.
        const auto common = reached();
        while (!finished && next_exchange <= common) {
            ++report.exchanges;
            // Compare every retained tick even if the exchange-point hashes agree: a
            // transient fault may have reconverged. Tick order precedes peer order.
            Divergence divergence;
            divergence.detected_at = next_exchange;
            bool found = false;
            for (const auto& [tick, hash] : states[0].hashes) {
                if (found || tick > next_exchange) break;
                for (std::size_t index = 1; index < count; ++index) {
                    const auto& other = states[index].hashes.at(tick);
                    const bool input_differs = states[index].input_hashes.at(tick) != states[0].input_hashes.at(tick);
                    if (other != hash || input_differs) {
                        divergence.completed_tick = tick;
                        divergence.peer = index;
                        divergence.channel = input_differs ? "input history" : "world/script";
                        divergence.hash_reference = input_differs ? states[0].input_hashes.at(tick) : hash;
                        divergence.hash_peer = input_differs ? states[index].input_hashes.at(tick) : other;
                        if (tick != 0) {
                            divergence.input_reference = states[0].inputs.at(tick);
                            divergence.input_peer = states[index].inputs.at(tick);
                        }
                        found = true;
                        break;
                    }
                }
            }
            if (found) {
                if (options.keep_encodings) {
                    divergence.encoding_reference = states[0].encodings.at(next_exchange);
                    divergence.encoding_peer = states[divergence.peer].encodings.at(next_exchange);
                    const auto& x = divergence.encoding_reference;
                    const auto& y = divergence.encoding_peer;
                    const auto shared = std::min(x.size(), y.size());
                    divergence.first_differing_byte = static_cast<std::size_t>(
                        std::mismatch(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(shared), y.begin()).first
                        - x.begin());
                }
                report.divergence = std::move(divergence);
                report.completed_ticks = common;
                return report;
            }
            const auto agreed = states[0].hashes.at(next_exchange);
            const auto agreed_input = states[0].input_hashes.at(next_exchange);
            for (auto& state : states) {
                state.hashes.erase(state.hashes.begin(), state.hashes.upper_bound(next_exchange));
                state.encodings.erase(state.encodings.begin(), state.encodings.upper_bound(next_exchange));
                // The agreed row stays as the base of the next search.
                state.hashes.emplace(next_exchange, agreed);
                state.input_hashes.erase(state.input_hashes.begin(), state.input_hashes.upper_bound(next_exchange));
                state.input_hashes.emplace(next_exchange, agreed_input);
                state.inputs.erase(state.inputs.begin(), state.inputs.upper_bound(next_exchange));
            }
            report.final_hash = agreed;
            if (next_exchange == final_tick) {
                finished = true;
            } else {
                next_exchange = std::min(next_exchange - next_exchange % every + every, final_tick);
            }
        }
        // Held batches still in flight are progress to come; anything else that stops is a deadlock.
        idle_rounds = progress ? 0 : idle_rounds + 1;
        if (!finished && !progress) {
            bool in_flight = false;
            for (const auto& state : states) {
                for (const auto& stream : state.inbox) in_flight = in_flight || !stream.empty();
            }
            if (!in_flight) {
                return fail(detail::loopback_error("deadlock: no peer can seal, receive or step at round "
                    + std::to_string(round)));
            }
            if (idle_rounds > options.max_wait_rounds) {
                return fail(detail::loopback_error("stall: no batch arrived for " + std::to_string(idle_rounds)
                    + " rounds at round " + std::to_string(round)));
            }
        }
    }
    for (std::size_t index = 0; index < count; ++index) {
        if (states[index].next_command != states[index].peer->commands.size()) {
            return fail(detail::loopback_error("peer " + std::to_string(index)
                + " has commands at or after the final tick " + std::to_string(final_tick)));
        }
    }
    report.completed_ticks = final_tick;
    return report;
}

// Tactical sessions (M2): commands are player commands, issuers are players.
struct TacticalAdapter {
    using Session = sim::tactical::TacticalSession;
    using Command = sim::tactical::PlayerCommand;

    [[nodiscard]] static std::uint64_t tick(const Command& command) noexcept { return command.key.tick; }
    [[nodiscard]] static std::uint64_t issuer(const Command& command) noexcept { return command.key.player_id; }
    [[nodiscard]] static std::uint64_t sequence(const Command& command) noexcept { return command.key.sequence; }
    [[nodiscard]] static core::Result<void> submit(Session& session, const Command& command) {
        return session.submit(command);
    }
    [[nodiscard]] static core::Result<std::string> step(Session& session, const sim::PartitionExecutor& executor) {
        auto tick = session.step(executor);
        if (!tick) return core::Result<std::string>::failure(tick.error());
        auto hash = tick.value().state_sha256.empty() ? tick.value().state_hash.get() : tick.value().state_sha256;
        return core::Result<std::string>::success(std::move(hash));
    }
    [[nodiscard]] static std::uint64_t completed_tick(const Session& session) noexcept {
        return session.completed_tick();
    }
    [[nodiscard]] static std::string state_hash(const Session& session) { return session.state_sha256(); }
    [[nodiscard]] static std::vector<std::uint8_t> encoding(const Session& session) {
        return session.canonical_state_bytes();
    }
    [[nodiscard]] static core::Result<std::vector<std::uint8_t>> input_bytes(const Session& session,
        const std::span<const Command> batch) {
        if (batch.empty()) return core::Result<std::vector<std::uint8_t>>::success({});
        auto replay = session.record_through_next_tick();
        replay.commands.assign(batch.begin(), batch.end());
        return sim::tactical::write_replay(replay);
    }
};

} // namespace eawr::testing::lockstep
