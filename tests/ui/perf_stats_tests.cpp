// #558: the performance overlay's model (frame times, tick costs, the rolling window, the graph
// columns). It counts samples and durations it is given, so no clock is involved.

#include "eawr/presentation/ui/perf_stats.hpp"
#include "ui_test_support.hpp"

#include <cmath>
#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace eawr;
namespace ui = presentation::ui;

void expect(const bool condition, const std::string& message) { test::ui::expect(condition, message.c_str()); }

[[nodiscard]] bool near(const double a, const double b) { return std::fabs(a - b) < 1.0e-6; }

void test_steady_frames() {
    ui::PerfStats stats;
    for (int i = 0; i < 120; ++i) stats.record_frame(1.0 / 60.0);
    const ui::PerfSummary summary = stats.summary();
    expect(std::fabs(summary.fps - 60.0) <= 1.5, "steady 60 Hz frames read as about 60 FPS");
    expect(near(summary.frame_ms, 1000.0 / 60.0) && near(summary.frame_avg_ms, 1000.0 / 60.0)
               && near(summary.frame_worst_ms, 1000.0 / 60.0),
           "steady frames: last, average and worst agree");
    expect(summary.frames == 120 && !summary.tick, "the frames are counted; no tick is invented");
}

void test_hitch_and_window() {
    ui::PerfStats stats;
    for (int i = 0; i < 60; ++i) stats.record_frame(0.010);
    stats.record_frame(0.150);
    for (int i = 0; i < 10; ++i) stats.record_frame(0.010);
    expect(near(stats.summary().frame_worst_ms, 150.0), "a hitch is the window's worst frame");
    // The hitch falls out of the 5 s window.
    for (int i = 0; i < 520; ++i) stats.record_frame(0.010);
    expect(near(stats.summary().frame_worst_ms, 10.0), "the hitch leaves the window after five seconds");
    expect(stats.summary().window_frames <= 501, "the window keeps about five seconds of frames");
}

void test_tick_cost() {
    ui::PerfStats stats;
    for (int i = 0; i < 30; ++i) {
        stats.record_frame(0.016);
        stats.record_tick({static_cast<std::uint64_t>(i + 1), 2.0, {{"step", 1.5}, {"fog", 0.5}}});
    }
    stats.record_frame(0.016);
    stats.record_tick({31, 40.0, {{"step", 39.0}, {"fog", 1.0}}});
    stats.record_frame(0.016);
    stats.record_tick({32, 2.5, {{"step", 2.0}, {"fog", 0.5}}});
    const ui::PerfSummary summary = stats.summary();
    expect(summary.tick && summary.tick->tick == 32 && near(summary.tick->total_ms, 2.5), "the newest tick is the last one");
    expect(summary.tick && summary.tick->phases.size() == 2 && summary.tick->phases[0].name == "step",
           "a tick keeps its named phases");
    expect(near(summary.tick_worst_ms, 40.0) && summary.ticks == 32, "the worst tick of the window is the spike");
    expect(summary.tick_avg_ms > 2.0 && summary.tick_avg_ms < 4.0, "the average tick cost is over the window");
    // Five seconds later the spike is gone.
    for (int i = 0; i < 400; ++i) stats.record_frame(0.016);
    expect(!stats.summary().tick && near(stats.summary().tick_worst_ms, 0.0), "ticks leave the window like frames");
}

void test_graph_keeps_spikes() {
    ui::PerfStats stats;
    for (int i = 0; i < 400; ++i) {
        stats.record_frame(0.0125);
        if (i == 100) stats.record_tick({1, 33.0, {}});
    }
    const std::vector<float> frames = stats.frame_graph(50);
    const std::vector<float> ticks = stats.tick_graph(50);
    expect(frames.size() == 50 && ticks.size() == 50, "a graph has the asked number of columns");
    float peak = 0.0F;
    for (const float value : ticks) peak = std::max(peak, value);
    expect(peak == 33.0F, "a spike survives when many samples share a column");
    expect(std::fabs(frames.back() - 12.5F) < 1.0e-3F, "the newest column holds the newest frames");
    const std::vector<float> empty = ui::PerfStats().frame_graph(8);
    expect(empty.size() == 8 && empty.front() == 0.0F, "an empty history draws flat");
    expect(ui::PerfStats().frame_graph(0).empty(), "no columns, no graph");
}

void test_bad_input() {
    ui::PerfStats stats;
    stats.record_frame(-1.0);
    stats.record_frame(std::nan(""));
    stats.record_frame(0.02);
    expect(stats.summary().frames == 3 && near(stats.summary().frame_ms, 20.0), "a negative or NaN duration counts as zero");
}

} // namespace

int main() {
    test_steady_frames();
    test_hitch_and_window();
    test_tick_cost();
    test_graph_keeps_spikes();
    test_bad_input();
    if (test::ui::failures() != 0) {
        std::cerr << test::ui::failures() << " perf stats check(s) failed\n";
        return 1;
    }
    std::cout << "ui perf stats passed\n";
    return 0;
}
