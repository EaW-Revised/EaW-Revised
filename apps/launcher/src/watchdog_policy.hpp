#pragma once

#include <cstdint>

namespace eawr::launcher {

// When the native watchdog may raise its Force close prompt. Windows calls a window "hung" after about five
// seconds without message pumping, but a busy manager (a big load notes block, a slow disk) recovers by itself
// within seconds. The prompt waits for a hang that lasts, so a short stall never alarms the player.
inline constexpr std::uint64_t kHungPromptAfterMs = 15000;
// The watchdog samples about once per second. A longer gap means this loop itself did not run (sleep, a
// suspended launcher), so earlier observations say nothing about the time in between.
inline constexpr std::uint64_t kHungSampleGapMs = 5000;

struct HangTracker {
    bool hung = false;           // The last sample was hung.
    std::uint64_t since_ms = 0;  // When the current continuous hang was first seen.
    std::uint64_t last_ms = 0;   // The last sample time.
};

struct HangVerdict {
    bool hung = false;           // Windows reports the window as not responding right now.
    std::uint64_t hung_ms = 0;   // How long it has been continuously hung as observed (0 when responding).
    bool prompt = false;         // Show the Force close prompt.
};

// Pure: feed one sample (`hung_now` from the OS, `now_ms` from a monotonic clock) and get the verdict.
// The prompt shows once the hang has lasted `prompt_after_ms`, and goes away the moment the window responds.
[[nodiscard]] inline HangVerdict observe_hang(HangTracker& tracker, bool hung_now, std::uint64_t now_ms,
                                              std::uint64_t prompt_after_ms = kHungPromptAfterMs) {
    const bool gap = tracker.last_ms != 0 && now_ms > tracker.last_ms && now_ms - tracker.last_ms > kHungSampleGapMs;
    tracker.last_ms = now_ms;
    if (!hung_now) {
        tracker.hung = false;
        return {};
    }
    if (!tracker.hung || gap) {
        tracker.hung = true;
        tracker.since_ms = now_ms;
    }
    const std::uint64_t duration = now_ms > tracker.since_ms ? now_ms - tracker.since_ms : 0;
    return {true, duration, duration >= prompt_after_ms};
}

} // namespace eawr::launcher
