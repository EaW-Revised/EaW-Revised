#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/fog_cells.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/victory.hpp"
#include "eawr/sim/tactical/visibility.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// A live tactical session for the game build (#80, docs/simulation.md "Game build"): the
// session steps on its own simulation thread, worker 0 of a #276 pool, and hands the caller
// only immutable snapshots. Orders enter through submit() as command values and become the
// next tick's replay input, so a live run records exactly the replay that reproduces it.
// This header names no session type: presentation may include it (UI-07 boundary).
namespace eawr::platform {

// Scripts that run beside the world (#79, live_scripts.hpp).
struct LiveScripts;

// The scripts' load and reports (#79): Lua instructions per tick and the distinct diagnostics.
struct LiveScriptReport {
    std::uint64_t ticks{};
    std::int64_t max_instructions{};   // in one tick, all instances
    std::int64_t total_instructions{};
    std::vector<std::string> diagnostics; // distinct "code message", first seen first, at most 64
};

// One order for the session's command queue (scripted and debug orders; a local player's input
// arrives through Options::command_source instead). The issuer is the commanding player; the
// units are nonzero and strictly increasing. Without `tick` the order is stamped with the next
// tick to execute when the simulation thread takes it; with one it is held until that tick is
// next, and a tick that has already executed rejects it (EAWR-SIM-0307). Held orders take their
// issuer's next sequence only when their tick comes, after that tick's command_source commands,
// so a preloaded later order never blocks the issuer's earlier input.
struct LiveOrder {
    sim::tactical::PlayerId issuer{};
    std::vector<sim::EntityId> units;
    sim::tactical::CommandPayload payload;
    std::optional<std::uint64_t> tick;
};

// The two newest published ticks and when the newest was published. `previous` equals
// `latest` until a second tick completes.
struct LiveFrame {
    std::shared_ptr<const sim::tactical::TacticalSnapshot> previous;
    std::shared_ptr<const sim::tactical::TacticalSnapshot> latest;
    std::chrono::steady_clock::time_point latest_published{};
};

// #558: what one completed tick cost the simulation thread, in wall-clock milliseconds. `phases`
// names the parts that add up to `total_ms` ("step": the session's tick, scripts included; "fog":
// the fog copy for the world); any producer of a sim phase timing appends its own entry. Presentation
// reads them for the performance overlay; they never enter a snapshot, a hash or the replay.
struct LivePhaseCost {
    std::string name;
    double ms{};
};
struct LiveTickCost {
    std::uint64_t tick{};
    double total_ms{};
    std::vector<LivePhaseCost> phases;
};

// #494: one player's fog cells after a completed tick (FogCells::values, row by row), for the fog
// drawn in the world. Immutable shared rows keep earlier ticks alive without copying cells.
struct LiveFog {
    std::uint64_t tick{};
    sim::tactical::FogRules rules{};
    sim::tactical::PlayerId player{};
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> values;
};

// The events of one completed tick that the battle view presents (LiveEventLog).
struct LiveTickEvents {
    std::uint64_t tick{};
    std::vector<sim::tactical::Event> events;
    std::vector<sim::tactical::CombatEvent> combat_events;
};

// What events_after() hands back: the ticks with presented events in (after, through], oldest
// first (ticks without any are left out), and, when the log's bounds dropped ticks with events
// in that range before they were read, the newest tick of the range they may have been in:
// never past `through`, since a dropped tick the reader did not ask for is not its loss.
struct LiveEvents {
    std::vector<LiveTickEvents> ticks;
    std::optional<std::uint64_t> lost_through;
};

// The session's presentation event log (#370): the events the battle view consumes, kept apart
// from the snapshot history so a presentation stall longer than that history still plays every
// hit and death. Only projectile hits (CombatEventKind::projectile_hit: impact effects),
// hardpoint and unit destructions (explosions, death clones) and the spin-aways of killed craft
// (#447: their start and end explosions) are kept; orders, target
// acquisitions, shots and victories are not, since the view reads shots and the outcome from
// the snapshots and the rest not at all. Two bounds, the oldest ticks going first past either:
// - `ticks`: only the events of the newest `ticks` completed ticks;
// - `bytes`: the kept records' size, counted as sizeof(LiveTickEvents) per kept tick plus
//   sizeof(Event) or sizeof(CombatEvent) per kept event (on x64: 56 + 40 or 104 bytes). A
//   tick whose own record exceeds it is dropped alone, the older ticks kept.
// The worst-case memory is therefore `bytes` of records plus the heap's per-block overhead of
// at most two vectors per kept tick (each kept tick counts at least 96 bytes, so the overhead
// stays below a third of `bytes`). A dropped tick that held events is remembered, so
// after() reports the gap instead of skipping it. Not thread-safe: LiveSession guards it.
class LiveEventLog final {
public:
    struct Bounds {
        std::size_t ticks{18000};           // ten minutes of battle at 30 ticks a second
        std::size_t bytes{std::size_t{16} << 20U}; // 16 MiB: about 160,000 hits
    };

    explicit LiveEventLog(Bounds bounds) noexcept;

    // Whether the battle view consumes the event.
    [[nodiscard]] static bool presented(const sim::tactical::Event& event) noexcept;
    [[nodiscard]] static bool presented(const sim::tactical::CombatEvent& event) noexcept;
    // The bytes a kept record counts against Bounds::bytes.
    [[nodiscard]] static std::size_t record_bytes(const LiveTickEvents& record) noexcept;

    // Keeps the presented events of the snapshot's tick, then applies the bounds. Snapshots
    // come in completed-tick order.
    void record(const sim::tactical::TacticalSnapshot& snapshot);
    // The kept ticks in (after, through], and the newest dropped tick in that range, if any.
    [[nodiscard]] LiveEvents after(std::uint64_t after, std::uint64_t through) const;
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

private:
    void drop_front();

    Bounds bounds_;
    std::deque<LiveTickEvents> records_; // oldest first
    std::size_t bytes_{};
    // The oldest and the newest dropped tick that had events: a range asked after the log
    // dropped them lost what lies between, conservatively.
    std::optional<std::uint64_t> dropped_from_;
    std::optional<std::uint64_t> dropped_through_;
};

class LiveSession final {
public:
    // WR-13: nonblocking preview query. Busy simulation returns no verdict; presentation
    // keeps its last preview until the next frame. The authoritative command always rechecks.
    [[nodiscard]] std::optional<bool> reinforcement_point(sim::tactical::PlayerId player,
        sim::tactical::TypeId type, const sim::math::Vec3& point) const;
    enum class Pacing : std::uint8_t {
        // The simulation thread keeps one tick due every wall-clock interval of the target rate
        // (1000 / target whole milliseconds, docs/behaviour/tactical-time-controls.md TM-02).
        // When it falls more than max_catch_up ticks behind it resets its clock instead of
        // spiralling.
        real_time,
        // The simulation thread steps only up to the tick advance_to() names, as fast as it
        // can. Captures and tests use it: which frame shows which tick no longer depends on
        // the host's speed.
        driven,
    };
    struct Options {
        // Threads of the pool, the simulation thread included (1 to 256).
        std::size_t workers{1};
        Pacing pacing{Pacing::real_time};
        // Real-time pacing's target in logical frames per wall-clock second (TM-01 to TM-03:
        // 10, 20, 30, 45, 60 or 120), 1 to 1000; set_target_rate() changes it.
        std::uint32_t target_rate{30};
        std::uint64_t max_catch_up{5};
        // Snapshots kept for snapshot_at(), newest first; at least 2.
        std::size_t history{64};
        // The event log's bounds (LiveEventLog): the ticks of presented events kept for
        // events_after(), apart from the snapshot history (at least `history`), and the bytes
        // those records may hold. The defaults keep ten minutes of battle and at most 16 MiB
        // of records (below 22 MiB with the heap's overhead), whatever the event rate.
        std::size_t event_history{LiveEventLog::Bounds{}.ticks};
        std::size_t event_bytes{LiveEventLog::Bounds{}.bytes};
        // Called on the simulation thread right before each step with the tick about to run
        // (UI-07: CommandScheduler::take). Its commands are submitted in order with the keys
        // they carry, never restamped; refusals go to rejected_orders(). It must outlive the
        // simulation thread: stop() the session before destroying what it reads.
        std::function<std::vector<sim::tactical::PlayerCommand>(std::uint64_t next_tick)> command_source{};
        // Scripts beside the world (#79): the world steps inside their scripted session, whose
        // commands join the replay; the tick hashes stay the world's.
        std::shared_ptr<const LiveScripts> scripts{};
        // #494: the player whose fog cells each kept tick carries (fog_at()); none keeps no fog history.
        std::optional<sim::tactical::PlayerId> fog_player{};
    };

    // The pool size of the game build: the hardware threads minus the Godot main and render
    // threads, at least one.
    [[nodiscard]] static std::size_t game_worker_count() noexcept;

    // Creates the session from its setup and content tables (TacticalSession::create) and
    // starts the simulation thread. The tick-zero snapshot is published before it returns.
    // `victory` is the skirmish's victory rules (#77); without them no outcome is decided.
    // `fog` binds the map's fog grid (#495); without it visibility is the exact range test.
    // `abilities` is its ability table (#76); without it no unit has abilities. A unit ability is
    // switched by an order whose payload is a sim::tactical::AbilityPayload (docs/behaviour/
    // space-abilities.md AB-50 for the command bar); the snapshots carry each instance's
    // AbilityStatus list.
    // `economy` is the skirmish's economy (#530); without it nothing is bought.
    [[nodiscard]] static core::Result<std::unique_ptr<LiveSession>> start(
        const sim::tactical::TacticalSetup& setup,
        std::span<const sim::tactical::SensorProfile> sensors,
        const sim::tactical::DurabilityTable& durability,
        const sim::tactical::MotionTable& motion,
        const sim::tactical::CombatTable& combat,
        Options options,
        const sim::tactical::VictoryRules& victory = sim::tactical::VictoryRules{},
        const std::optional<sim::tactical::FogRules>& fog = std::nullopt,
        const sim::tactical::AbilityTable& abilities = sim::tactical::AbilityTable{},
        const sim::tactical::EconomyRules& economy = sim::tactical::EconomyRules{});

    ~LiveSession();
    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    // Thread-safe. Queues the order for the simulation thread, which submits it before its
    // next step. A rejected order is reported by rejected_orders(), not here.
    void submit(LiveOrder order);
    // Driven pacing: lets the simulation thread step until `tick` has completed.
    void advance_to(std::uint64_t tick);
    // #459 time controls (docs/behaviour/tactical-time-controls.md). Thread-safe; they change
    // only when ticks run, never what a tick does (TP-02). While paused no tick runs in either
    // pacing; orders still queue for the next tick (TM-10). A new target rate, like a resume,
    // restarts the real-time clock at the current tick, so no tick is made up or skipped
    // (TP-01). Rates outside 1 to 1000 are clamped.
    void set_paused(bool paused);
    void set_target_rate(std::uint32_t frames_per_second);
    [[nodiscard]] bool paused() const;
    // #453 (docs/behaviour/battle-end.md BEP-02): the session never steps past `tick`; a later
    // call can only lower the limit. wait_for() on a later tick then times out.
    void halt_at(std::uint64_t tick);
    [[nodiscard]] std::optional<std::uint64_t> halt_tick() const;
    // Blocks until `tick` has completed, the session failed or stopped, or `timeout` passed.
    [[nodiscard]] bool wait_for(std::uint64_t tick, std::chrono::milliseconds timeout) const;

    [[nodiscard]] LiveFrame frame() const;
    // A kept snapshot of that completed tick, or null.
    [[nodiscard]] std::shared_ptr<const sim::tactical::TacticalSnapshot> snapshot_at(std::uint64_t tick) const;
    // #494: Options::fog_player's cells after that kept tick, or null (no fog_player, a session
    // without fog rules, or a tick that left the history).
    [[nodiscard]] std::shared_ptr<const LiveFog> fog_at(std::uint64_t tick) const;
    // The presented events of the completed ticks in (after, through], oldest first (LiveEventLog).
    [[nodiscard]] LiveEvents events_after(std::uint64_t after, std::uint64_t through) const;
    [[nodiscard]] std::uint64_t completed_tick() const;
    // #558: the cost of the completed ticks after `after`, oldest first; only the newest
    // tick_cost_history ticks are kept.
    static constexpr std::size_t tick_cost_history = 4096;
    [[nodiscard]] std::vector<LiveTickCost> tick_costs_after(std::uint64_t after) const;
    // Set once a step fails or the simulation thread throws (the exception is caught there,
    // never escapes the thread); the simulation thread then stops stepping.
    [[nodiscard]] std::optional<core::Diagnostic> failure() const;
    // Orders the session refused at submission, and the warnings of executed commands whose
    // unit orders were rejected, in the order they happened.
    [[nodiscard]] std::vector<core::Diagnostic> rejected_orders() const;
    [[nodiscard]] std::size_t worker_count() const noexcept;
    // The scripts' load and diagnostics so far; empty without scripts.
    [[nodiscard]] LiveScriptReport script_report() const;

    // Stops and joins the simulation thread; idempotent. The session keeps its last state.
    void stop();
    // After stop(): the state hash of every completed tick, tick 1 first, and the replay of
    // the run (setup, final tick = completed tick, every submitted command).
    [[nodiscard]] std::vector<std::string> tick_hashes() const;
    [[nodiscard]] sim::tactical::TacticalReplay record() const;
    // Once failure() is set (#615): the replay through the failed tick, its commands included
    // (TacticalSession::record_through_next_tick), which a headless run replays into the same
    // failure. Safe while the stopped simulation thread is still joined or not.
    [[nodiscard]] sim::tactical::TacticalReplay failure_record() const;

private:
    class Impl;
    explicit LiveSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// The per-tick state hashes of a replay run headless on the calling thread (InlineExecutor)
// with the given content tables: what a live run's hashes must equal (#80).
[[nodiscard]] core::Result<std::vector<std::string>> headless_tick_hashes(
    const sim::tactical::TacticalReplay& replay,
    std::span<const sim::tactical::SensorProfile> sensors,
    const sim::tactical::DurabilityTable& durability,
    const sim::tactical::MotionTable& motion,
    const sim::tactical::CombatTable& combat,
    const sim::tactical::VictoryRules& victory = sim::tactical::VictoryRules{},
    const std::optional<sim::tactical::FogRules>& fog = std::nullopt,
    const sim::tactical::AbilityTable& abilities = sim::tactical::AbilityTable{},
    const sim::tactical::EconomyRules& economy = sim::tactical::EconomyRules{});

} // namespace eawr::platform
