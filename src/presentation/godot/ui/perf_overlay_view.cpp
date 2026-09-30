#include "perf_overlay_view.hpp"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <iomanip>
#include <sstream>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

// The graph's columns cover the model's five-second window; the model keeps the worst sample of
// each, so a one-frame spike stays one visible bar.
constexpr std::size_t graph_columns = 150;

const Color good_colour(0.35F, 0.95F, 0.45F, 1.0F);
const Color warn_colour(1.0F, 0.75F, 0.2F, 1.0F);
const Color bad_colour(1.0F, 0.3F, 0.25F, 1.0F);
const Color text_colour(0.95F, 0.95F, 0.95F, 1.0F);
const Color dim_colour(0.7F, 0.72F, 0.75F, 1.0F);

[[nodiscard]] String format(const char* pattern, ...) {
    char buffer[96];
    va_list arguments;
    va_start(arguments, pattern);
    std::vsnprintf(buffer, sizeof(buffer), pattern, arguments);
    va_end(arguments);
    return String(buffer);
}

// A tick or frame is comfortable within half of its budget, late up to the budget, over it after.
[[nodiscard]] Color cost_colour(const double ms) {
    if (ms <= model::perf_frame_budget_ms + 0.5) return good_colour;
    return ms <= model::perf_tick_budget_ms + 0.5 ? warn_colour : bad_colour;
}

[[nodiscard]] Color fps_colour(const double fps) {
    if (fps >= 55.0) return good_colour;
    return fps >= 28.0 ? warn_colour : bad_colour;
}

template <std::size_t N>
[[nodiscard]] double nice_scale(const double worst, const std::array<double, N>& steps) {
    for (const double step : steps) {
        if (worst <= step) return step;
    }
    return steps.back();
}

constexpr std::array<double, 7> frame_scales{20.0, 40.0, 80.0, 160.0, 320.0, 640.0, 1280.0};
constexpr std::array<double, 10> tick_scales{2.0, 5.0, 10.0, 20.0, 40.0, 80.0, 160.0, 320.0, 640.0, 1280.0};

} // namespace

EawrPerfOverlay::EawrPerfOverlay() {
    set_mouse_filter(MOUSE_FILTER_IGNORE);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
    set_visible(false);
}

void EawrPerfOverlay::setup(Ref<Font> font, std::string face) {
    font_ = std::move(font);
    face_ = std::move(face);
}

void EawrPerfOverlay::set_shown(const bool shown) {
    if (shown_ == shown) return;
    shown_ = shown;
    ++toggles_;
    set_visible(shown);
    if (shown) {
        // A hidden spell records nothing: start from a fresh window.
        stats_ = model::PerfStats();
        last_frame_.reset();
        since_refresh_ = 1.0;
        since_redraw_ = 1.0;
        rebase_ = true;
        text_summary_ = {};
        cost_sum_ms_ = cost_max_ms_ = 0.0;
        cost_samples_ = 0;
        queue_redraw();
    }
}

void EawrPerfOverlay::frame(const std::vector<model::PerfTick>& ticks, const std::optional<std::size_t> visible_units,
                            const bool tick_source) {
    if (!shown_) return;
    const auto start = Clock::now();
    const double seconds = last_frame_ ? std::chrono::duration<double>(start - *last_frame_).count() : 0.0;
    last_frame_ = start;
    rebase_ = false;
    if (seconds > 0.0) stats_.record_frame(seconds);
    for (const model::PerfTick& tick : ticks) stats_.record_tick(tick);
    tick_source_ = tick_source;
    visible_units_ = visible_units;
    since_refresh_ += seconds;
    if (since_refresh_ >= 0.1) {
        since_refresh_ = 0.0;
        refresh_text();
    }
    // The canvas keeps the last draw commands, so the panel is rebuilt at 20 Hz, not every frame:
    // its columns are 33 ms wide, and the model keeps each column's worst sample.
    since_redraw_ += seconds;
    if (since_redraw_ >= 0.05) {
        since_redraw_ = 0.0;
        queue_redraw();
    }
    frame_cost_ms_ = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    // Every shown frame is one sample; a redraw adds its cost on top.
    cost_sum_ms_ += frame_cost_ms_;
    cost_max_ms_ = std::max(cost_max_ms_, frame_cost_ms_);
    ++cost_samples_;
}

void EawrPerfOverlay::refresh_text() {
    text_summary_ = stats_.summary();
    if (RenderingServer* server = RenderingServer::get_singleton()) {
        draw_calls_ = static_cast<std::uint64_t>(server->get_rendering_info(RenderingServer::RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME));
    }
}

void EawrPerfOverlay::_notification(const int what) {
    if (what != NOTIFICATION_DRAW || !shown_) return;
    const auto start = Clock::now();
    draw_overlay();
    const double drawn = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    cost_sum_ms_ += drawn;
    cost_max_ms_ = std::max(cost_max_ms_, frame_cost_ms_ + drawn);
}

void EawrPerfOverlay::draw_overlay() {
    const Vector2 view = get_size();
    Ref<Font> font = font_.is_valid() ? font_ : ThemeDB::get_singleton()->get_fallback_font();
    if (font.is_null() || view.y <= 0.0F) return;
    // 13 px at 720 lines, 39 px at 2160 (4K): the panel grows with the view.
    const int big = std::clamp(static_cast<int>(std::lround(view.y / 720.0F * 13.0F)), 12, 48);
    const int small = std::max(10, static_cast<int>(std::lround(static_cast<float>(big) * 0.8F)));
    font_size_ = big;
    const auto unit = static_cast<float>(big);
    const float margin = unit * 0.6F;
    const float pad = unit * 0.6F;
    const float width = unit * 22.0F;
    const float graph_height = unit * 3.4F;
    const float gap = unit * 0.5F;
    const float big_row = font->get_height(big) * 1.05F;
    const float small_row = font->get_height(small) * 1.05F;
    const float height = pad * 2.0F + (big_row + small_row + graph_height + gap) * 2.0F + small_row + small_row
        + (tick_source_ ? small_row : 0.0F);
    panel_rect_ = Rect2(margin, margin, width, height);
    draw_rect(panel_rect_, Color(0.0F, 0.0F, 0.0F, 0.62F), true);
    draw_rect(panel_rect_, Color(1.0F, 1.0F, 1.0F, 0.18F), false, 1.0F);

    const float left = margin + pad;
    const float inner = width - 2.0F * pad;
    float y = margin + pad;
    const auto text = [&](const String& value, const int size, const Color& colour, const HorizontalAlignment align) {
        draw_string(font, Vector2(left, y + font->get_ascent(size)), value, align, inner, size, colour);
    };
    const auto graph = [&](const std::vector<float>& values, const double scale) {
        const Rect2 box(left, y, inner, graph_height);
        draw_rect(box, Color(1.0F, 1.0F, 1.0F, 0.06F), true);
        for (const double reference : {model::perf_frame_budget_ms, model::perf_tick_budget_ms}) {
            if (reference >= scale) continue;
            const float line = box.position.y + box.size.y * (1.0F - static_cast<float>(reference / scale));
            draw_line(Vector2(box.position.x, line), Vector2(box.position.x + box.size.x, line),
                      Color(1.0F, 1.0F, 1.0F, 0.28F), 1.0F);
        }
        const float column = box.size.x / static_cast<float>(values.size());
        PackedVector2Array points;
        PackedColorArray colours;
        for (std::size_t index = 0; index < values.size(); ++index) {
            const float value = values[index];
            if (value <= 0.0F) continue;
            const float bar = std::max(1.0F, box.size.y * std::min(1.0F, static_cast<float>(value / scale)));
            const float x = box.position.x + (static_cast<float>(index) + 0.5F) * column;
            points.push_back(Vector2(x, box.position.y + box.size.y));
            points.push_back(Vector2(x, box.position.y + box.size.y - bar));
            colours.push_back(cost_colour(value));
        }
        if (colours.size() > 0) draw_multiline_colors(points, colours, std::max(1.0F, column));
        draw_string(font, Vector2(box.position.x + 3.0F, box.position.y + font->get_ascent(small)),
                    format("%.0f ms", scale), HORIZONTAL_ALIGNMENT_LEFT, -1.0F, small, Color(1.0F, 1.0F, 1.0F, 0.55F));
    };

    const model::PerfSummary& now = text_summary_;
    // Frames.
    text(format("%.0f FPS", now.fps), big, fps_colour(now.fps), HORIZONTAL_ALIGNMENT_LEFT);
    text(format("%.1f ms", now.frame_ms), big, cost_colour(now.frame_ms), HORIZONTAL_ALIGNMENT_RIGHT);
    y += big_row;
    text(format("avg %.1f ms", now.frame_avg_ms), small, dim_colour, HORIZONTAL_ALIGNMENT_LEFT);
    text(format("worst %.1f ms / 5 s", now.frame_worst_ms), small, dim_colour, HORIZONTAL_ALIGNMENT_RIGHT);
    y += small_row;
    graph(stats_.frame_graph(graph_columns), nice_scale(now.frame_worst_ms, frame_scales));
    y += graph_height + gap;
    // The simulation's tick cost.
    text("Sim tick", big, text_colour, HORIZONTAL_ALIGNMENT_LEFT);
    if (tick_source_ && now.tick) {
        text(format("%.2f ms", now.tick->total_ms), big, cost_colour(now.tick->total_ms), HORIZONTAL_ALIGNMENT_RIGHT);
    } else {
        text(tick_source_ ? "waiting" : "no live session", small, dim_colour, HORIZONTAL_ALIGNMENT_RIGHT);
    }
    y += big_row;
    text(format("avg %.2f ms", now.tick_avg_ms), small, dim_colour, HORIZONTAL_ALIGNMENT_LEFT);
    text(format("worst %.2f ms / 5 s", now.tick_worst_ms), small, dim_colour, HORIZONTAL_ALIGNMENT_RIGHT);
    y += small_row;
    if (tick_source_) {
        String phases;
        if (now.tick) {
            for (const model::PerfPhase& phase : now.tick->phases) {
                if (!phases.is_empty()) phases += "  ";
                phases += String(phase.name.c_str()) + format(" %.2f", phase.ms);
            }
        }
        text(phases, small, dim_colour, HORIZONTAL_ALIGNMENT_LEFT);
        y += small_row;
    }
    graph(stats_.tick_graph(graph_columns), nice_scale(now.tick_worst_ms, tick_scales));
    y += graph_height + gap;
    // Render counts, and the overlay's own cost.
    text(format("draw calls %llu", static_cast<unsigned long long>(draw_calls_)), small, dim_colour, HORIZONTAL_ALIGNMENT_LEFT);
    text(visible_units_ ? format("units %zu", *visible_units_) : String("units -"), small, dim_colour,
         HORIZONTAL_ALIGNMENT_RIGHT);
    y += small_row;
    const double average = cost_samples_ > 0 ? cost_sum_ms_ / static_cast<double>(cost_samples_) : 0.0;
    text(format("overlay %.3f ms", average), small, Color(1.0F, 1.0F, 1.0F, 0.5F), HORIZONTAL_ALIGNMENT_LEFT);
    text(format("%s hides", key_name), small, Color(1.0F, 1.0F, 1.0F, 0.5F), HORIZONTAL_ALIGNMENT_RIGHT);
}

std::string EawrPerfOverlay::report_json() const {
    const model::PerfSummary summary = stats_.summary();
    std::ostringstream output;
    output << std::fixed << std::setprecision(3);
    output << "{\"key\": \"" << key_name << "\", \"shown\": " << (shown_ ? "true" : "false")
           << ", \"toggles\": " << toggles_ << ", \"font\": \"" << face_ << "\", \"font_size\": " << font_size_
           << ", \"frames\": " << summary.frames
           << ", \"fps\": " << summary.fps << ", \"frame_ms\": " << summary.frame_ms
           << ", \"frame_ms_avg\": " << summary.frame_avg_ms << ", \"frame_ms_worst\": " << summary.frame_worst_ms
           << ", \"tick_source\": \"" << (tick_source_ ? "live_session" : "none") << "\""
           << ", \"ticks\": " << summary.ticks << ", \"tick_ms\": " << (summary.tick ? summary.tick->total_ms : 0.0)
           << ", \"tick_ms_avg\": " << summary.tick_avg_ms << ", \"tick_ms_worst\": " << summary.tick_worst_ms
           << ", \"tick_phases\": [";
    if (summary.tick) {
        for (std::size_t index = 0; index < summary.tick->phases.size(); ++index) {
            const model::PerfPhase& phase = summary.tick->phases[index];
            output << (index == 0 ? "" : ", ") << "{\"name\": \"" << phase.name << "\", \"ms\": " << phase.ms << "}";
        }
    }
    output << "], \"draw_calls\": " << draw_calls_ << ", \"visible_units\": ";
    if (visible_units_) output << *visible_units_;
    else output << "null";
    output << ", \"own_cost_ms\": {\"samples\": " << cost_samples_
           << ", \"avg\": " << (cost_samples_ > 0 ? cost_sum_ms_ / static_cast<double>(cost_samples_) : 0.0)
           << ", \"max\": " << cost_max_ms_ << "}, \"rect\": [" << panel_rect_.position.x << ", " << panel_rect_.position.y
           << ", " << panel_rect_.size.x << ", " << panel_rect_.size.y << "]}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
