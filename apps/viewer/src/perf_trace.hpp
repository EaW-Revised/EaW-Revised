#pragma once

#include <cstdint>
#include <chrono>
#include <fstream>
#include <optional>
#include <string>

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "particle_adapter.hpp"

namespace eawr::presentation::godot_backend {

// --eawr-perf-trace <csv> (#601, docs/performance/battle-bench.md): one row per drawn frame of a live
// session with the frame's wall-clock time, the engine's process time (the viewer's own frame work,
// the live session's main-thread side included), the renderer's measured CPU and GPU time of the root
// viewport, its draw calls, objects and primitives, and what the frame showed: the presented tick,
// the units the local player sees, the projectiles in flight and the live battle-effect and unit
// emitter particles. A development measurement only: it reads timers and counters, never a command,
// a snapshot's state or a hash. The #558 performance overlay shows live values on screen; this
// writes the whole fight for the benchmark report.
class PerfTrace final {
public:
    struct Frame final {
        double frame_ms{};
        std::uint64_t presented_tick{};
        std::uint64_t units{};
        std::uint64_t projectiles{};
        std::uint64_t effects{};
        std::uint64_t effect_particles{};
        std::uint64_t emitter_particles{};
        // #638: the main thread's wall-clock ms in the frame's unit emitters, battle effects and
        // breakoff props (stepping their particle systems, building and uploading the streams).
        double particle_ms{};
        double emitter_prepare_ms{};
        double projectile_prepare_ms{};
        double clone_prepare_ms{};
        double tick_wait_ms{};
        double live_frame_ms{};
        double session_tail_ms{};
        double clip_pose_ms{};
        double emitter_frame_ms{};
        double effects_frame_ms{};
        double debris_frame_ms{};
        double idle_frame_ms{};
        double idle_sample_ms{};
        double idle_upload_ms{};
        GodotParticleBackend::FrameWork particle_work{};
        double bookkeeping_ms{};
        double hud_ms{};
        double audio_ms{};
        double fog_ms{};
        double pose_ms{};
        double compose_ms{};
        double refresh_ms{};
        double opacity_ms{};
        // #638: the simulation ticks completed since the previous row and their summed cost
        // (platform::LiveTickCost::total_ms), so a run shows whether the frame slowed the tick.
        std::uint64_t ticks{};
        double tick_ms{};
        // #957: of those ticks, the summed and the worst single tick's wall-clock ms in the AI's
        // barrier step and in the Lua service (the cost phases "ai" and "lua"), 0 without scripts.
        double ai_ms{};
        double ai_worst_ms{};
        double lua_ms{};
        double lua_worst_ms{};
        // #888: the main thread's wall-clock ms in the space view's snapshot build and renderer
        // submit, and the pieces that submit carried.
        double submit_ms{};
        std::uint64_t pieces{};
        std::uint64_t sent{}; // of those, the transforms sent to the engine
    };

    // The path of --eawr-perf-trace, nothing without it, or an error text.
    [[nodiscard]] static std::optional<std::string> parse(const godot::PackedStringArray& arguments, std::string& error);

    [[nodiscard]] bool open(const std::string& path, std::string& error);
    // Monotonic entry-to-entry and extension/external intervals; engine delta and
    // the delayed Godot TIME_PROCESS maximum are separate observations.
    void begin_frame();
    // Appends the frame; the renderer's numbers are those of the frame drawn before this one.
    void frame(const Frame& frame);
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

private:
    std::ofstream output_;
    godot::RID viewport_;
    std::uint64_t frames_{};
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> previous_entry_;
    std::optional<Clock::time_point> previous_report_end_;
    Clock::time_point frame_entry_{};
    double wall_frame_ms_{};
    double outside_viewer_ms_{};
};

} // namespace eawr::presentation::godot_backend
