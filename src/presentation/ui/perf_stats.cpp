#include "eawr/presentation/ui/perf_stats.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace eawr::presentation::ui {

namespace {

// The worst value per column of `columns` covering (now - window, now], oldest column first.
template <class Samples, class Value>
std::vector<float> graph(const Samples& samples, const std::size_t columns, const double now, const double window,
                         const Value& value) {
    std::vector<float> out(columns, 0.0F);
    if (columns == 0 || window <= 0.0) return out;
    const double width = window / static_cast<double>(columns);
    for (const auto& sample : samples) {
        const double age = now - sample.time; // 0 is the newest instant
        if (age < 0.0 || age >= window) continue;
        const auto from_end = static_cast<std::size_t>(age / width);
        const std::size_t column = columns - 1 - std::min(from_end, columns - 1);
        out[column] = std::max(out[column], static_cast<float>(value(sample)));
    }
    return out;
}

} // namespace

PerfStats::PerfStats(const double window_seconds) : window_(window_seconds > 0.0 ? window_seconds : perf_window_seconds) {}

void PerfStats::record_frame(const double seconds) {
    const double dt = std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
    if (frames_ == 0) first_ = now_;
    now_ += dt;
    frame_samples_.push_back({now_, dt * 1000.0});
    ++frames_;
    trim();
}

void PerfStats::record_tick(PerfTick tick) {
    tick_samples_.push_back({now_, std::move(tick)});
    ++ticks_;
    trim();
}

void PerfStats::trim() {
    while (!frame_samples_.empty() && now_ - frame_samples_.front().time >= window_) frame_samples_.pop_front();
    while (!tick_samples_.empty() && now_ - tick_samples_.front().time >= window_) tick_samples_.pop_front();
}

PerfSummary PerfStats::summary() const {
    PerfSummary out;
    out.frames = frames_;
    out.ticks = ticks_;
    out.window_frames = frame_samples_.size();
    out.window_ticks = tick_samples_.size();
    if (!frame_samples_.empty()) {
        double sum = 0.0;
        for (const FrameSample& sample : frame_samples_) {
            sum += sample.ms;
            out.frame_worst_ms = std::max(out.frame_worst_ms, sample.ms);
        }
        out.frame_ms = frame_samples_.back().ms;
        out.frame_avg_ms = sum / static_cast<double>(frame_samples_.size());
        // FPS: the frames of the last second, over that second (or over the run so far, shorter).
        std::size_t recent = 0;
        for (auto it = frame_samples_.rbegin(); it != frame_samples_.rend() && now_ - it->time < perf_fps_seconds; ++it) {
            ++recent;
        }
        const double span = std::min(perf_fps_seconds, now_ - first_);
        if (span > 0.0) out.fps = static_cast<double>(recent) / span;
    }
    if (!tick_samples_.empty()) {
        double sum = 0.0;
        for (const TickSample& sample : tick_samples_) {
            sum += sample.tick.total_ms;
            out.tick_worst_ms = std::max(out.tick_worst_ms, sample.tick.total_ms);
        }
        out.tick = tick_samples_.back().tick;
        out.tick_avg_ms = sum / static_cast<double>(tick_samples_.size());
    }
    return out;
}

std::vector<float> PerfStats::frame_graph(const std::size_t columns) const {
    return graph(frame_samples_, columns, now_, window_, [](const FrameSample& sample) { return sample.ms; });
}

std::vector<float> PerfStats::tick_graph(const std::size_t columns) const {
    return graph(tick_samples_, columns, now_, window_, [](const TickSample& sample) { return sample.tick.total_ms; });
}

} // namespace eawr::presentation::ui
