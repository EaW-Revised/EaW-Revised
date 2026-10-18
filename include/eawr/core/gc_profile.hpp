#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::gc_profile {

// Process-local diagnostics. Never saved, hashed, or used to make a decision.
inline bool requested() {
#ifdef _MSC_VER
    char value[2]{};
    std::size_t length{};
    return getenv_s(&length, value, sizeof(value), "EAWR_PROFILE_GALACTIC_AI") == 0
        && std::string_view(value) == "1";
#else
    const auto* value = std::getenv("EAWR_PROFILE_GALACTIC_AI");
    return value && std::string_view(value) == "1";
#endif
}
inline std::atomic<bool> enabled{requested()};
enum class Phase : std::size_t {
    ai_barrier_wall, scripted_hash_wall, frame_end_hash_wall, native_digest,
    lua_sync, make_snapshot, snapshot_ai, snapshot_other,
    runtime_prepare, world_bind,
    // S3a attribution scopes (serial, wall): nested under runtime_prepare / world_bind.
    prepare_freestore, prepare_reservations, prepare_connectivity, publish_routes, barrier_begin,
    world_compare, world_copy, world_catalog, world_producers, world_rows, forces, world_objects,
    world_visibility, world_economy, world_observed, world_input, world_view, world_release,
    compare_production, view_moved, freestore_publish, selection, goal_record_maintenance, usability_record_maintenance,
    barrier_commit, layer_replay, shadow_view, policy_snapshot,
    // Tick-cost attribution scopes (serial, wall): the galactic session step and the script host around it.
    tick_galaxy_step, tick_commands, tick_discovery, tick_gather, tick_movement, tick_control, tick_income,
    tick_production, tick_conflict_tail, tick_fiscal, tick_victory, tick_population, tick_state_hash,
    tick_actor_delivery, tick_story_service, tick_game_step,
    snap_sorted, snap_observer, snap_menus, snap_economy_input, snap_forecast, snap_corruption, snap_offers, snap_construct,
    perception, perception_record_maintenance, perception_lock_wait, economy_lock_wait, count
};
inline constexpr std::array<std::string_view, static_cast<std::size_t>(Phase::count)> names{
    "ai_barrier_wall", "scripted_hash_wall", "frame_end_hash_wall", "native_digest",
    "lua_sync", "make_snapshot", "snapshot_ai", "snapshot_other",
    "runtime_prepare", "world_bind",
    "prepare_freestore", "prepare_reservations", "prepare_connectivity", "publish_routes", "barrier_begin",
    "world_compare", "world_copy", "world_catalog", "world_producers", "world_rows", "forces", "world_objects",
    "world_visibility", "world_economy", "world_observed", "world_input", "world_view", "world_release",
    "compare_production", "view_moved", "freestore_publish", "selection", "goal_record_maintenance", "usability_record_maintenance",
    "barrier_commit", "layer_replay", "shadow_view", "policy_snapshot",
    "tick_galaxy_step", "tick_commands", "tick_discovery", "tick_gather", "tick_movement", "tick_control", "tick_income",
    "tick_production", "tick_conflict_tail", "tick_fiscal", "tick_victory", "tick_population", "tick_state_hash",
    "tick_actor_delivery", "tick_story_service", "tick_game_step",
    "snap_sorted", "snap_observer", "snap_menus", "snap_economy_input", "snap_forecast", "snap_corruption", "snap_offers", "snap_construct",
    "perception", "perception_record_maintenance", "perception_lock_wait", "economy_lock_wait"};
struct Values { std::uint64_t calls{}, inclusive_ns{}, exclusive_ns{}; };
struct Counter { std::atomic<std::uint64_t> calls{}, inclusive_ns{}, exclusive_ns{}; };
inline std::array<Counter, names.size()> counters{};
using Snapshot = std::array<Values, names.size()>;
inline thread_local bool in_ai{};
class Scope;
inline thread_local Scope* current{};

class Scope final {
public:
    explicit Scope(Phase phase) noexcept : phase_(phase), active_(enabled.load(std::memory_order_relaxed)) {
        if (!active_) return;
        parent_ = current;
        current = this;
        start_ = std::chrono::steady_clock::now();
    }
    Scope(Phase fallback, Phase ai_context) noexcept : Scope(fallback) {
        if (active_ && in_ai) phase_ = ai_context;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        if (!active_) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_).count();
        const auto ns = elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
        current = parent_;
        if (parent_) parent_->children_ns_ += ns;
        auto& row = counters[static_cast<std::size_t>(phase_)];
        row.calls.fetch_add(1, std::memory_order_relaxed);
        row.inclusive_ns.fetch_add(ns, std::memory_order_relaxed);
        row.exclusive_ns.fetch_add(ns > children_ns_ ? ns - children_ns_ : 0, std::memory_order_relaxed);
    }
private:
    Phase phase_;
    bool active_;
    Scope* parent_{};
    std::uint64_t children_ns_{};
    std::chrono::steady_clock::time_point start_{};
};

template<class Function> auto timed(Phase phase, Function&& function) {
    Scope scope(phase);
    return std::forward<Function>(function)();
}
template<class Function> auto timed_ai(Function&& function) {
    if (!enabled.load(std::memory_order_relaxed)) return std::forward<Function>(function)();
    struct Context final {
        bool previous{in_ai};
        Context() { in_ai = true; }
        ~Context() { in_ai = previous; }
    } context;
    Scope scope(Phase::ai_barrier_wall);
    return std::forward<Function>(function)();
}
// Probe readers expose only diagnostic counters, never live game state. Keeping
// them beside the shared header scopes avoids a new engine-library dependency.
inline Snapshot snapshot() noexcept {
    Snapshot values{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto& row = counters[i];
        values[i] = {row.calls.load(std::memory_order_relaxed), row.inclusive_ns.load(std::memory_order_relaxed),
            row.exclusive_ns.load(std::memory_order_relaxed)};
    }
    return values;
}
// Read at a quiescent driven-frame boundary; no concurrent reset is supported.
inline Snapshot difference(const Snapshot& begin, const Snapshot& end) noexcept {
    Snapshot values{};
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = {end[i].calls - begin[i].calls, end[i].inclusive_ns - begin[i].inclusive_ns,
            end[i].exclusive_ns - begin[i].exclusive_ns};
    return values;
}
inline double milliseconds(const Snapshot& frame, Phase phase) noexcept {
    return static_cast<double>(frame[static_cast<std::size_t>(phase)].inclusive_ns) / 1000000.0;
}
// Direct, nonoverlapping S0 subtotal; worker-summed phases are not budget inputs.
inline double compute_ms(const Snapshot& frame) noexcept {
    return milliseconds(frame, Phase::ai_barrier_wall) - milliseconds(frame, Phase::snapshot_ai)
        + milliseconds(frame, Phase::native_digest) + milliseconds(frame, Phase::lua_sync);
}
inline double hash_residual_ms(const Snapshot& frame) noexcept {
    return milliseconds(frame, Phase::scripted_hash_wall) + milliseconds(frame, Phase::frame_end_hash_wall)
        - milliseconds(frame, Phase::native_digest) - milliseconds(frame, Phase::lua_sync);
}
inline void write_frame(std::ostream& out, const Snapshot& frame, std::uint64_t tick) {
    out << "{\"tick\":" << tick << ",\"compute_subtotal_ms\":" << compute_ms(frame)
        << ",\"hash_wall_residual_ms\":" << hash_residual_ms(frame) << ",\"wall_phases\":{";
    constexpr auto cpu_begin = static_cast<std::size_t>(Phase::perception);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        if (i == cpu_begin) out << "},\"worker_summed_cpu_phases\":{";
        out << (i && i != cpu_begin ? "," : "") << '"' << names[i] << "\":{\"calls\":" << frame[i].calls
            << ",\"inclusive_ns\":" << frame[i].inclusive_ns << ",\"exclusive_ns\":" << frame[i].exclusive_ns << '}';
    }
    out << "}}";
}
inline void write_budget(std::ostream& out, const std::vector<Snapshot>& frames) {
    out << "{\"estimator\":\"direct_scoped_subtotal\",\"target_p50_ms\":2,\"target_p95_ms\":8";
    if (frames.empty()) { out << ",\"samples\":0,\"p50_ms\":null,\"p95_ms\":null}"; return; }
    std::vector<double> compute;
    compute.reserve(frames.size());
    double residual{};
    for (const auto& frame : frames) { compute.push_back(compute_ms(frame)); residual += hash_residual_ms(frame); }
    std::sort(compute.begin(), compute.end());
    const auto p50 = compute[(compute.size() * 50 + 99) / 100 - 1];
    const auto p95 = compute[(compute.size() * 95 + 99) / 100 - 1];
    out << ",\"samples\":" << frames.size() << ",\"p50_ms\":" << p50 << ",\"p95_ms\":" << p95
        << ",\"within_subtotal_target\":" << (p50 <= 2.0 && p95 <= 8.0 ? "true" : "false")
        << ",\"hash_wall_residual_mean_ms\":" << residual / static_cast<double>(frames.size())
        << ",\"replay_compute_residual_ms\":null,\"caveat\":\"Scoped subtotal is not total AI compute: AI-context snapshots include temporary work; hash residual is not isolated AI work. Worker-summed CPU phases are separate. No paired replay; shared-host load and asynchronous hash waits affect wall times. Startup, save and restore are excluded.\"}";
}
} // namespace eawr::gc_profile
