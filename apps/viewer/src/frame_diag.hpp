#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rid.hpp>

#include "eawr/core/perf_diag/feed_writer.hpp"
#include "eawr/core/perf_diag/frame.hpp"
#include "eawr/core/perf_diag/marker.hpp"
#include "eawr/core/perf_diag/thread_cpu.hpp"
#include "eawr/core/tick_profile.hpp"

namespace eawr::presentation::godot_backend {

// Frame side diagnostics (#8058 P3), a write-only observer of the main thread, the render thread and the GPU. Off (the
// default) costs one bool test per frame: no clock read, no callback queued, no allocation, no lock. On
// (--eawr-frame-diag, later the launcher/settings key of the recorder), per frame:
//  - two steady_clock reads (entry, exit) on the main thread;
//  - one callback queued with RenderingServer::call_on_render_thread. It runs ON the render thread and reads the root
//    viewport's measured render CPU/GPU time there, where the server answers directly: unlike the main thread getters
//    of PerfTrace it does not make the Separate render thread synchronize with the main thread. It pushes a record into a
//    single producer ring; the main thread drains it next frame and joins it to the frame by ENGINE FRAME NUMBER
//    (core::perf_diag::FrameMatcher), never by arrival;
//  - the session-mutex wait of frame() / tick_costs_after() and the other accessors, read from the tick_profile
//    recording the frame already keeps for PerfTrace;
//  - the stale-frame flag LiveSessionView sets where the presented tick is clamped at the newest published tick;
//  - the Godot pipeline compilation monitors (main thread Performance reads, no render thread access), as markers.
// Nothing here is read by the simulation, a hash, a save or the network.
class FrameDiag final {
public:
    [[nodiscard]] static FrameDiag& instance();

    // --eawr-frame-diag turns it on; --eawr-frame-diag-csv <path> also writes the joined frames (and the raw render
    // samples to <path>.render.csv) when the run ends; --eawr-frame-diag-calibrate additionally reads the render times
    // through the old main-thread getters (this DOES stall the render thread: only for comparing the getters).
    void parse(const godot::PackedStringArray& arguments);

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void set_enabled(const bool on);

    // What the frame showed, for the telemetry feed fight-intensity columns (counts only; main thread).
    struct FightCounts final {
        std::uint32_t units{}, enemy_units{}, attacking{}, projectiles{}, effects{}, particles{};
    };

    // Main thread, frame entry / exit. begin_frame is one bool test while off. `fight` (optional) is what the frame
    // showed; the feed carries it next to the frame times.
    void begin_frame();
    void end_frame(bool stale, bool running, const core::tick_profile::Recording& ui_recording,
                   const FightCounts* fight = nullptr);
    // A named main-thread phase of the open frame that was measured outside the tick_profile recording (a frame timer).
    // Summed per name into the phase row of the frame in the telemetry feed. Main thread; a no-op without a feed.
    void note_phase(std::string_view name, double ms);
    // True while a telemetry feed receives this frame side (frame timers then run even without --eawr-perf-trace).
    [[nodiscard]] bool feed_active() const noexcept { return feed_; }

    struct Summary final {
        std::uint64_t frames{};          // frames recorded on the main thread
        std::uint64_t matched{};         // frames that got render CPU parts
        std::uint64_t incomplete{};      // frames that left the window without CPU
        std::uint64_t late{};            // render samples that came after their frame left the window
        std::uint64_t dropped{};         // render samples the ring overwrote
        std::uint64_t stale_frames{};
        std::uint64_t pipeline_compiles{};
        std::uint64_t gpu_samples{}, gpu_repeated{};
        double frame_ms_sum{}, viewer_main_ms_sum{}, outside_ms_sum{}, lock_wait_ms_sum{};
        double render_cpu_ms_sum{}, render_gpu_ms_sum{};
        double lock_wait_ms_max{}, render_cpu_ms_max{}, render_gpu_ms_max{};
        double own_cost_ms_sum{};        // main thread cost of this recorder (begin + end)
    };
    [[nodiscard]] const Summary& summary() const noexcept { return summary_; }
    // The "frame_diag" object of a map report ("null" while off).
    [[nodiscard]] std::string report_json() const;
    // Writes the CSV files named by --eawr-frame-diag-csv; called when the run ends.
    void write_files();

private:
    FrameDiag();
    static void render_thread_sample(std::int64_t tag);
    void on_matched(const core::perf_diag::FrameSample& sample);
    void push_feed_frame(const core::perf_diag::FrameSample& sample);
    void push_feed_phases(const core::tick_profile::Recording& ui_recording);
    void drain_rings();
    void read_pipeline_monitors(std::uint64_t frame);

    using Clock = std::chrono::steady_clock;
    bool enabled_{};
    bool calibrate_{};
    bool started_{};
    std::string csv_path_;
    godot::RID viewport_;
    std::uint64_t frame_{};            // engine frame number of the open frame
    Clock::time_point entry_{}, previous_entry_{}, previous_exit_{};
    bool have_previous_{};
    double begin_cost_ms_{};
    core::perf_diag::FrameMatcher<64> matcher_;
    core::perf_diag::SpscRing<core::perf_diag::Marker> markers_{256};
    std::array<double, 5> pipeline_previous_{};
    Summary summary_{};
    float previous_gpu_ms_{};
    bool have_previous_gpu_{};
    std::vector<core::perf_diag::FrameSample> history_;     // capped; only kept with a csv path
    struct Old final { std::uint64_t frame{}; double cpu{}, gpu{}; };
    std::vector<core::perf_diag::RenderSample> raw_;   // csv path only
    std::vector<Old> old_;                             // calibration only
    core::perf_diag::FrameRings rings_{1024};
    // The telemetry feed side: per-frame extras joined to the matched frame by number, the phase name ids and the main
    // thread label. The matcher emits a frame one or more frames after its end, so the extras wait in a window.
    struct Extra final {
        std::uint64_t frame{~0ULL};
        FightCounts fight{};
        bool fight_valid{};
    };
    std::array<Extra, 64> extras_{};
    std::map<std::string, core::perf_diag::PhaseId, std::less<>> phase_ids_;
    std::vector<std::pair<core::perf_diag::PhaseId, float>> frame_phase_;  // reused every frame
    std::optional<core::perf_diag::ThreadLabel> main_label_;
    float latest_gpu_ms_{-1.0F};
    bool feed_{};                      // a telemetry feed runs: frames and phases go to it
    static constexpr std::size_t history_cap = 200000;
};

} // namespace eawr::presentation::godot_backend
