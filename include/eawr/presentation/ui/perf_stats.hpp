#pragma once

// #558: the model of the viewer's performance overlay: frame times, the simulation's tick
// costs and their rolling window. It is presentation state read from wall-clock timers; it
// never enters a command, a snapshot, the replay or a hash. Time here is the sum of the
// recorded frame durations, so the model is deterministic and testable without a clock.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::ui {

// The graphs and the "worst" figures cover this many seconds.
inline constexpr double perf_window_seconds = 5.0;
// FPS is the frames of the last second.
inline constexpr double perf_fps_seconds = 1.0;
// One logical frame is 1/30 s; a tick that costs more than this cannot keep the nominal rate.
inline constexpr double perf_tick_budget_ms = 1000.0 / 30.0;
// 60 Hz frame budget: the graph's reference line.
inline constexpr double perf_frame_budget_ms = 1000.0 / 60.0;

// A named part of one tick's cost (any sim phase timing).
struct PerfPhase final {
    std::string name;
    double ms{};
};

struct PerfTick final {
    std::uint64_t tick{};
    double total_ms{};
    std::vector<PerfPhase> phases;
};

struct PerfSummary final {
    double fps{};
    double frame_ms{};       // the last frame
    double frame_avg_ms{};   // over the window
    double frame_worst_ms{}; // over the window
    std::size_t frames{};    // frames recorded in total
    std::size_t window_frames{};
    std::optional<PerfTick> tick; // the newest tick's cost
    double tick_avg_ms{};    // over the window
    double tick_worst_ms{};  // over the window
    std::size_t ticks{};     // ticks recorded in total
    std::size_t window_ticks{};
};

class PerfStats final {
public:
    explicit PerfStats(double window_seconds = perf_window_seconds);

    // One presented frame that took `seconds` of wall-clock time since the previous one.
    void record_frame(double seconds);
    // One completed tick's cost, stamped at the current time.
    void record_tick(PerfTick tick);

    [[nodiscard]] PerfSummary summary() const;
    // The graphs: `columns` values covering the window, oldest first, each the worst sample
    // that fell in its column (so a spike survives however many frames share a column) and 0
    // where no sample fell. A column is window/columns seconds wide, ending now.
    [[nodiscard]] std::vector<float> frame_graph(std::size_t columns) const;
    [[nodiscard]] std::vector<float> tick_graph(std::size_t columns) const;

    [[nodiscard]] double now() const noexcept { return now_; }
    [[nodiscard]] double window_seconds() const noexcept { return window_; }

private:
    struct FrameSample final {
        double time{};
        double ms{};
    };
    struct TickSample final {
        double time{};
        PerfTick tick;
    };
    void trim();

    double window_;
    double now_{};
    double first_{};
    std::size_t frames_{};
    std::size_t ticks_{};
    std::deque<FrameSample> frame_samples_;
    std::deque<TickSample> tick_samples_;
};

} // namespace eawr::presentation::ui
