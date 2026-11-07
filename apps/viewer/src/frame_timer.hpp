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

// Per-stage sums over a frame: mark(i) ADDS the ms since the previous mark/restart to slots[i]. A null array reads no clock.
class SumLapTimer final {
public:
    explicit SumLapTimer(double* slots) : slots_(slots) { if (slots_) last_ = Clock::now(); }
    void restart() { if (slots_) last_ = Clock::now(); }
    void mark(const std::size_t index) {
        if (!slots_) return;
        const auto now = Clock::now();
        slots_[index] += std::chrono::duration<double, std::milli>(now - last_).count();
        last_ = now;
    }
private:
    using Clock = std::chrono::steady_clock;
    double* slots_{};
    Clock::time_point last_{};
};

} // namespace eawr::presentation::godot_backend
