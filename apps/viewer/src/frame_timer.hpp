#pragma once

#include <chrono>

namespace eawr::presentation::godot_backend {

// Opt-in wall-clock laps for --eawr-perf-trace. No clock reads in ordinary play.
class FrameTimer final {
public:
    explicit FrameTimer(double* output) : output_(output) {
        if (output_) start_ = Clock::now();
    }
    FrameTimer(const FrameTimer&) = delete;
    FrameTimer& operator=(const FrameTimer&) = delete;
    ~FrameTimer() { finish(); }
    void finish() {
        if (!output_) return;
        *output_ = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        output_ = nullptr;
    }
private:
    using Clock = std::chrono::steady_clock;
    double* output_{};
    Clock::time_point start_{};
};

} // namespace eawr::presentation::godot_backend
