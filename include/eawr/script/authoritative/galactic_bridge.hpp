#pragma once

// Galactic sibling of the tactical script bridge (ADR-012, ADR-013).
// Script callbacks read the stepped galaxy and stage typed commands for the
// next session tick. AI and story have separate scheduler sets; all instance
// identities are globally unique across the two hosts.

#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/galactic/session.hpp"

#include <functional>
#include <cstddef>
#include <memory>
#include <map>
#include <set>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::authoritative {

// Coordinator-reserved: next after the engine adapter producer (16).
inline constexpr std::uint32_t galactic_story_producer = first_simulation_producer + 1;

struct GalacticOrder {
    sim::galactic::SeatId seat{};
    sim::galactic::CommandPayload payload;
};
using GalacticCommandTranslator = std::function<core::Result<GalacticOrder>(const ScriptCommand&)>;

struct RoutedGalacticCommand {
    ScriptCommand command;
    std::optional<sim::galactic::CommandOrderKey> key;
    std::optional<core::Diagnostic> dropped;
    std::optional<sim::galactic::GameCommand> accepted;
};

struct ScriptedGalacticTick {
    sim::galactic::GalacticTick world;
    std::vector<core::Diagnostic> refused_input;
    std::vector<RoutedGalacticCommand> script_input;
    std::optional<ServiceReport> ai; // absent while paused
    ServiceReport story;            // one service per recorded session barrier
    sim::StateHash state_hash;
};

// The script section's barrier data for checkpoint composition. The parent
// owns the galaxy checkpoint and adapter state; shared scheduler blobs own the
// Lua graphs. Pending orders and routing cursors must travel with those graphs.
struct GalacticScriptSave {
    std::uint64_t completed_tick{};
    std::uint64_t galactic_frame{};
    std::string ai;
    std::string story;
    std::vector<ScriptCommand> pending;
    std::map<sim::galactic::SeatId, std::uint64_t> next_sequence;
    std::set<sim::galactic::SeatId> exhausted_seats;
    std::string world_sha256;
    std::string scripts_sha256;
};

// Mode-aware engine seam: bounded ordered preparation, snapshot queries and
// events before the shared script-instances phase. The full AI and story graph
// reducers remain separate adapters. Engines use their own event producers and
// derive decisions from the supplied world and scheduler state; hidden mutable
// adapter state needs its own save/hash contribution in the whole-game host.
// Story engines leave pump/LastService events to this bridge.
// Suspended plot activity gates graph event
// reduction, never attached Lua service (WST-16).
class GalacticScriptEngine {
public:
    virtual ~GalacticScriptEngine() = default;
    [[nodiscard]] virtual bool uses_sync_hash() const noexcept { return false; }
    [[nodiscard]] virtual bool verifies_sync_hash() const noexcept { return false; }
    // Generic hosts follow world service. A retained galactic manager may admit
    // due planning alone under processing suppression during auto-resolve.
    [[nodiscard]] virtual bool admits_service(const sim::galactic::GalacticTick& tick) const noexcept {
        return tick.work.serviced;
    }
    [[nodiscard]] virtual core::Result<ServiceOptions> before_service(
        const sim::PartitionExecutor& executor, const sim::galactic::GalacticSession& world,
        const sim::galactic::GalacticTick& tick, ScriptScheduler& scripts) = 0;
    [[nodiscard]] virtual core::Result<void> after_service(ServiceReport&) {
        return core::Result<void>::success();
    }
};

class ScriptedGalacticSession final {
public:
    // Fresh world and fresh GC-profile schedulers, with a 1/30 second frame.
    // Restoring the full container belongs to checkpoint section wiring.
    [[nodiscard]] static core::Result<ScriptedGalacticSession> create(
        sim::galactic::GalacticSession world, ScriptScheduler ai, ScriptScheduler story);
    // The parent supplies the galaxy restored to the same barrier and fresh
    // schedulers with matching config, modules and bindings. Register the same
    // pure translators/engines before continuing, as for create().
    [[nodiscard]] static core::Result<ScriptedGalacticSession> restore(
        sim::galactic::GalacticSession world, ScriptScheduler ai, ScriptScheduler story, const GalacticScriptSave& saved);
    // Parent ownership transfer is transactional: failed validation retains its world.
    [[nodiscard]] static core::Result<ScriptedGalacticSession> restore_preserving(
        sim::galactic::GalacticSession& world, ScriptScheduler ai, ScriptScheduler story, const GalacticScriptSave& saved);
    ScriptedGalacticSession(ScriptedGalacticSession&&) noexcept;
    ScriptedGalacticSession& operator=(ScriptedGalacticSession&&) noexcept;
    ~ScriptedGalacticSession();

    [[nodiscard]] core::Result<void> register_verb(std::string_view verb, GalacticCommandTranslator translator);
    // WGM-154: staged orbit test through an explicitly selected script seat.
    [[nodiscard]] core::Result<void> bind_orbit_recheck(sim::galactic::SeatId seat);
    [[nodiscard]] core::Result<void> set_ai_engine(std::shared_ptr<GalacticScriptEngine> engine);
    [[nodiscard]] core::Result<void> set_story_engine(std::shared_ptr<GalacticScriptEngine> engine);
    void set_state_hasher(std::shared_ptr<sim::StateHasher> hasher) noexcept;
    [[nodiscard]] core::Result<ScriptedGalacticTick> step(const sim::PartitionExecutor& executor,
        std::span<const sim::galactic::GameCommand> input = {}, bool local_conflict_pending = false);

    // Parent-only mutation at layer/control barriers; scripts still read immutable snapshots.
    [[nodiscard]] sim::galactic::GalacticSession& parent_world() noexcept;
    [[nodiscard]] const sim::galactic::GalacticSession& world() const noexcept;
    // Borrowed parent scheduler; the enclosing session owns its lifetime.
    [[nodiscard]] ScriptScheduler& ai_scheduler() noexcept;
    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] std::size_t pending_script_commands() const noexcept;
    [[nodiscard]] sim::galactic::GalacticReplay record() const;
    [[nodiscard]] core::Result<std::string> script_state_hash();
    [[nodiscard]] core::Result<std::string> script_sync_hash();
    [[nodiscard]] bool uses_sync_hash() const noexcept;
    // Separate shared-scheduler persistence blobs, at the completed barrier.
    [[nodiscard]] core::Result<std::string> save_ai();
    [[nodiscard]] core::Result<std::string> save_story();
    [[nodiscard]] core::Result<GalacticScriptSave> save_scripts();

private:
    struct Impl;
    explicit ScriptedGalacticSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// WGM-154 staged adapter. Register before instances; the parent must bind its
// script seat with bind_orbit_recheck. Synchronous result/wait remains a named seam.
[[nodiscard]] core::Result<void> register_galactic_orbit_recheck_method(
    ScriptScheduler& scripts, std::uint32_t object_handle_kind);

// Install named EAWR-SCRIPT-0215 faults for the inventory's missing global
// engine functions. A host instead registers its implemented adapter under
// those names before creating instances. Methods need the adapter's reserved
// handle kind and use register_missing_galactic_method.
[[nodiscard]] core::Result<void> register_missing_galactic_api(
    ScriptScheduler& scripts, std::span<const std::string_view> implemented = {});
[[nodiscard]] core::Result<void> register_missing_galactic_method(
    ScriptScheduler& scripts, std::uint32_t handle_kind, std::string_view method);

} // namespace eawr::script::authoritative
