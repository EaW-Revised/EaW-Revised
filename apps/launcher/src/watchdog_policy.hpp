#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace eawr::launcher {

// When the native watchdog may raise its Force close prompt. A busy manager (a big load notes block, a slow
// disk) recovers by itself within seconds. The prompt waits for a hang that lasts (consecutive failed probes),
// so a short stall never alarms the player.
inline constexpr std::uint64_t kHungPromptAfterMs = 15000;
// The watchdog samples about once per second. A longer gap means this loop itself did not run (sleep, a
// suspended launcher), so earlier observations say nothing about the time in between.
inline constexpr std::uint64_t kHungSampleGapMs = 5000;

struct HangTracker {
    bool hung = false;           // The last sample was hung.
    std::uint64_t since_ms = 0;  // When the current continuous hang was first seen.
    std::uint64_t last_ms = 0;   // The last sample time.
};

// A launcher that is still hung this long after the prompt came up is ended without asking: it is only the mod
// manager (a running game is a separate job and is not touched), the player may be away from the screen, and the
// frozen window can otherwise sit there for hours (a GPU reset left it waiting on the graphics driver for good).
inline constexpr std::uint64_t kHungAutoEndAfterPromptMs = 60000;

struct HangVerdict {
    bool hung = false;           // The last probe of the window timed out.
    std::uint64_t hung_ms = 0;   // How long it has been continuously hung as observed (0 when responding).
    bool prompt = false;         // Show the Force close prompt.
    bool auto_end = false;       // End the hung launcher now: still hung kHungAutoEndAfterPromptMs after the prompt.
};

// How the prompt window comes up. While a game runs it must never take the focus away from it (a fullscreen game
// would be minimised or lose its input): the window then only shows and flashes its taskbar button. With no game
// it is activated so the player sees it at once.
enum class PromptShow { activate, flash_only };
[[nodiscard]] constexpr PromptShow choose_prompt_show(bool game_running) {
    return game_running ? PromptShow::flash_only : PromptShow::activate;
}

// How long one probe waits for the window to answer a no-op message before the probe counts as failed.
inline constexpr std::uint64_t kProbeTimeoutMs = 2000;

// What one probe of one window found. Only a real timeout says the window is not answering: Windows' own
// "not responding" flag is never consulted, because an idle Godot window (which peeks its queue with filtered
// ranges) is flagged by it within seconds while it answers messages in milliseconds.
enum class ProbeResult {
    answered,    // The window handled the message.
    timeout,     // The message was not handled within kProbeTimeoutMs.
    window_gone, // The window closed or cannot be probed (destroyed, access denied): says nothing about a hang.
};

struct ProbeSummary {
    bool hung = false;           // At least one probed window timed out.
    std::size_t hung_index = 0;  // Which window (index into the probed list) timed out first.
    bool answered = false;       // No window timed out and at least one actually answered (zero windows, or only
                                 // closed ones, is not an answer).
};

// Pure: probe each window in order with `probe(handle)` and stop at the first one that times out.
template <typename Handle, typename Probe>
[[nodiscard]] ProbeSummary first_unanswered(const std::vector<Handle>& windows, Probe&& probe) {
    bool any_answer = false;
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const ProbeResult result = probe(windows[index]);
        if (result == ProbeResult::timeout) return {true, index, false};
        if (result == ProbeResult::answered) any_answer = true;
    }
    return {false, 0, any_answer};
}

// Pure: feed one sample (`hung_now` from the probe, `now_ms` from a monotonic clock) and get the verdict.
// The prompt shows once the hang has lasted `prompt_after_ms`, and goes away the moment the window responds.
[[nodiscard]] inline HangVerdict observe_hang(HangTracker& tracker, bool hung_now, std::uint64_t now_ms,
                                              std::uint64_t prompt_after_ms = kHungPromptAfterMs,
                                              std::uint64_t auto_end_after_prompt_ms = kHungAutoEndAfterPromptMs) {
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
    return {true, duration, duration >= prompt_after_ms, duration >= prompt_after_ms + auto_end_after_prompt_ms};
}

// What the last probes said about how fast the window answers, written into watchdog.json each sample so a
// report can tell a real stall (a long probe, a growing time since the last answer) from a false "not
// responding" flag (probes answered in milliseconds).
struct ProbeTiming {
    bool probed = false;                 // At least one probe has finished.
    std::uint64_t last_probe_ms = 0;     // How long the last finished probe took (the timeout length when it timed out).
    std::uint64_t last_answer_at_ms = 0; // When a probe last got an answer (monotonic clock); 0 when none yet.
};

// Pure: record one finished probe (`latency_ms`, `answered`, `now_ms` from a monotonic clock).
inline void note_probe(ProbeTiming& timing, bool answered, std::uint64_t latency_ms, std::uint64_t now_ms) {
    timing.probed = true;
    timing.last_probe_ms = latency_ms;
    if (answered) timing.last_answer_at_ms = now_ms;
}

// Pure: time since a probe last got an answer, 0 when none has yet (nothing to measure from).
[[nodiscard]] inline std::uint64_t since_answer_ms(const ProbeTiming& timing, std::uint64_t now_ms) {
    return timing.last_answer_at_ms != 0 && now_ms > timing.last_answer_at_ms ? now_ms - timing.last_answer_at_ms : 0;
}

} // namespace eawr::launcher
