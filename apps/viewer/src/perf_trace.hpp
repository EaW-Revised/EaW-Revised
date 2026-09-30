#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rid.hpp>

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
    };

    // The path of --eawr-perf-trace, nothing without it, or an error text.
    [[nodiscard]] static std::optional<std::string> parse(const godot::PackedStringArray& arguments, std::string& error);

    [[nodiscard]] bool open(const std::string& path, std::string& error);
    // Appends the frame; the renderer's numbers are those of the frame drawn before this one.
    void frame(const Frame& frame);
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }

private:
    std::ofstream output_;
    godot::RID viewport_;
    std::uint64_t frames_{};
};

} // namespace eawr::presentation::godot_backend
