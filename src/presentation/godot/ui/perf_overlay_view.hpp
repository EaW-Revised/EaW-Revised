#pragma once

// #558: the performance overlay (docs/ui/perf-overlay.md). A dev tool drawn over the viewer's
// view, off by default: FPS and frame time with a rolling graph of the last five seconds, the
// simulation's cost per tick with the window's worst tick and its own graph, and, when the
// caller has them, the draw calls and visible units. It reads wall-clock timers only, ignores
// the pointer and never touches the simulation or its hashes. While hidden it does nothing.

#include "eawr/presentation/ui/perf_stats.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/variant/color.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class EawrPerfOverlay final : public godot::Control {
    GDCLASS(EawrPerfOverlay, godot::Control)

public:
    // The toggle key, named for the docs and the report.
    static constexpr const char* key_name = "F3";

    EawrPerfOverlay();
    // `face` names the font for the report ("engine font" when the cache had none).
    void setup(godot::Ref<godot::Font> font, std::string face);
    void set_shown(bool shown);
    [[nodiscard]] bool shown() const noexcept { return shown_; }
    // Whether the next frame() forgets the ticks before it (after a hidden spell).
    [[nodiscard]] bool wants_rebase() const noexcept { return rebase_; }

    // One presented frame. `ticks` are the costs of the simulation ticks completed since the
    // last call (empty when no session runs); `visible_units` is the count the caller draws.
    void frame(const std::vector<presentation::ui::PerfTick>& ticks, std::optional<std::size_t> visible_units,
               bool tick_source);
    void _notification(int what);

    // The report's "perf_overlay" object.
    [[nodiscard]] std::string report_json() const;
    [[nodiscard]] presentation::ui::PerfSummary summary() const { return stats_.summary(); }

protected:
    static void _bind_methods() {}

private:
    using Clock = std::chrono::steady_clock;
    void draw_overlay();
    void refresh_text();

    godot::Ref<godot::Font> font_;
    std::string face_;
    bool shown_{};
    bool rebase_{true};
    bool tick_source_{};
    std::optional<std::size_t> visible_units_;
    presentation::ui::PerfStats stats_;
    std::optional<Clock::time_point> last_frame_;
    double since_refresh_{1.0};
    double since_redraw_{1.0};
    presentation::ui::PerfSummary text_summary_;
    std::uint64_t draw_calls_{};
    int font_size_{};
    godot::Rect2 panel_rect_;
    std::uint64_t toggles_{};
    // The overlay's own CPU cost per shown frame: frame() plus the draw commands built that frame.
    double cost_sum_ms_{};
    double cost_max_ms_{};
    std::uint64_t cost_samples_{};
    double frame_cost_ms_{};
};

} // namespace eawr::presentation::godot_backend
