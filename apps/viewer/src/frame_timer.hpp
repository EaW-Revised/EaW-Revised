#pragma once

#include <chrono>

namespace eawr::presentation::godot_backend {

// Opt-in wall-clock laps for --eawr-perf-trace. No clock reads in ordinary play.
class FrameTimer final {
public:
    explicit FrameTimer(double* output, bool accumulate = false) : output_(output), accumulate_(accumulate) {
        if (output_) start_ = Clock::now();
    }
    FrameTimer(const FrameTimer&) = delete;
    FrameTimer& operator=(const FrameTimer&) = delete;
    ~FrameTimer() { finish(); }
    void finish() {
        if (!output_) return;
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        if (accumulate_) *output_ += elapsed;
        else *output_ = elapsed;
        output_ = nullptr;
    }
private:
    using Clock = std::chrono::steady_clock;
    double* output_{};
    bool accumulate_{};
    Clock::time_point start_{};
};

} // namespace eawr::presentation::godot_backend
