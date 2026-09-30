#pragma once

// Script commands as replay input (#247, docs/lua-sandbox.md "Command routing"):
// one authoritative tick of a tactical session that runs scripts. The world
// steps first; the scripts are serviced after it, seeing its result; the
// commands they issue are translated into tactical player commands keyed for
// the next tick and submitted through TacticalSession::submit like any
// player's input, so the session's replay records them and a headless replay
// of that record reproduces the world without running a script. No script
// call touches world storage, and nothing outside step() services a script.

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::authoritative {

// A script command in tactical terms: the commanding player, the units and the
// order. The key is assigned by the router, never by the translator.
struct TacticalOrder {
    sim::tactical::PlayerId issuer{};
    std::vector<sim::EntityId> units;
    sim::tactical::CommandPayload payload;
};

// Turns one script command into a tactical order. Registered per verb by the
// binding API that issues the verb (#79 registers FoC's). It must be a pure
// function of the command. A failure, or an exception it throws, drops the
// command with EAWR-SCRIPT-0214 before anything reaches the world.
using CommandTranslator = std::function<core::Result<TacticalOrder>(const ScriptCommand&)>;

// A script command as tick input: the key it was submitted with, or why it
// was dropped.
struct RoutedCommand {
    ScriptCommand command;
    bool submitted{};
    sim::CommandKey key;       // submitted
    core::Diagnostic dropped;  // otherwise
};

struct ScriptedTick {
    sim::tactical::TacticalTick world;
    // Player input the world refused, in submission order; it is not recorded.
    std::vector<core::Diagnostic> refused_input;
    // The previous service's commands, submitted as this tick's input in
    // commit order.
    std::vector<RoutedCommand> script_input;
    // This tick's service; its commands are the next tick's script input.
    ServiceReport scripts;
    // authoritative_state_sha256 of the completed tick: world and script state. Empty with a
    // state hasher (set_state_hasher); then `state_hash` has it.
    std::string state_sha256;
    // The same hash either way (#637): ready, or pending on the state hasher.
    sim::StateHash state_hash;
};

// The engine side of the scripts (#79): what the original engine does to its
// script instances each frame besides pumping them, such as calling their
// service functions, setting globals and delivering world events. It runs
// serially after the world step and before the script service, reads the
// stepped world, submits this tick's engine events to the scheduler and says
// which instances it paces (ServiceOptions). It must be a pure function of the
// world and the script state, so a replay of the tick reproduces it.
class ScriptEngine {
public:
    virtual ~ScriptEngine() = default;
    [[nodiscard]] virtual core::Result<ServiceOptions> before_service(
        const sim::tactical::TacticalSession& world, const sim::tactical::TacticalTick& tick, ScriptScheduler& scripts) = 0;
    // Serially after the script service: the engine takes out of `report.commands` the
    // commands addressed to itself (requests to engine objects, such as a TaskForce's
    // production, #449); the rest become the next tick's world input. Like before_service it
    // must be a pure function of the world, the script state and the report.
    [[nodiscard]] virtual core::Result<void> after_service(ServiceReport& report) {
        static_cast<void>(report);
        return core::Result<void>::success();
    }
};

class ScriptedTacticalSession {
public:
    // The scheduler and the world must be at the same completed tick (both
    // fresh, or both restored at one barrier).
    [[nodiscard]] static core::Result<ScriptedTacticalSession> create(
        sim::tactical::TacticalSession world, ScriptScheduler scripts);
    ScriptedTacticalSession(ScriptedTacticalSession&&) noexcept;
    ScriptedTacticalSession& operator=(ScriptedTacticalSession&&) noexcept;
    ~ScriptedTacticalSession();

    // Before the first step. A verb without a translator is dropped with
    // EAWR-SCRIPT-0214 when a script issues it.
    [[nodiscard]] core::Result<void> register_verb(std::string_view verb, CommandTranslator translator);
    // Before the first step; without one every instance is pumped every tick.
    [[nodiscard]] core::Result<void> set_engine(std::shared_ptr<ScriptEngine> engine);
    // #637: hashes the world (TacticalSession::set_state_hasher) and derives the tick's combined
    // hash on `hasher`, off the stepping thread. Null (the default) hashes on it.
    void set_state_hasher(std::shared_ptr<sim::StateHasher> hasher) noexcept;

    // One tick. Submits `player_input` with the keys it carries (UI-07
    // CommandScheduler::take; a refused command is reported and dropped),
    // then the previous service's script commands in commit order, each
    // translated and given its issuer's next sequence at this tick (one past
    // the last key accepted from either path; untranslatable or refused ones
    // are dropped with EAWR-SCRIPT-0214); steps the world; then services the
    // scripts on the same executor. A failed world step or script service
    // is terminal: the session refuses further steps.
    [[nodiscard]] core::Result<ScriptedTick> step(
        const sim::PartitionExecutor& executor, std::span<const sim::tactical::PlayerCommand> player_input = {});

    [[nodiscard]] const sim::tactical::TacticalSession& world() const noexcept;
    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    // Script state hash at the tick barrier (ScriptScheduler::state_hash).
    [[nodiscard]] core::Result<std::string> script_state_hash();
    // Script commands routed to the next tick and not yet submitted.
    [[nodiscard]] std::size_t pending_script_commands() const noexcept;
    // The world's replay: setup, every executed player and script command.
    [[nodiscard]] sim::tactical::TacticalReplay record() const;

private:
    struct Impl;
    explicit ScriptedTacticalSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::script::authoritative
