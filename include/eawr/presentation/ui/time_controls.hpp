#pragma once

// #459: the tactical time panel (docs/behaviour/tactical-time-controls.md). The pause and
// fast-forward buttons and the speed setting decide how many logical frames run per
// wall-clock second, or none; nothing here changes what a tick does (TP-02). The model is
// presentation state: it never enters a command, the replay or a hash. Each change is
// recorded in the time track (TP-04) at the tick the next step would run.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

enum class TimeState : std::uint8_t { play, paused, fast_forward };

[[nodiscard]] std::string_view to_string(TimeState state) noexcept;

// TM-01: the five tactical speed steps' targets in logical frames per wall-clock second.
inline constexpr std::array<std::uint32_t, 5> speed_step_rates{10, 20, 30, 45, 60};
inline constexpr std::uint32_t default_speed_step = 2;
// TM-03: fast forward's target.
inline constexpr std::uint32_t fast_forward_rate = 120;
// TR-01: one logical frame is 1/30 s of game time.
inline constexpr std::uint32_t nominal_rate = 30;

// One entry of the time track: from `tick` (the next tick to run when the change was made)
// on, the state and its target rate (0 while paused or ended).
struct TimeChange {
    std::uint64_t tick{};
    TimeState state{TimeState::play};
    std::uint32_t target_rate{};
    std::string cause; // "pause", "play", "fast_forward", "end", ...
};

class TimeControls final {
public:
    // `speed_step` 0 to 4 (TM-01; larger values are clamped).
    explicit TimeControls(std::uint32_t speed_step = default_speed_step);

    // The pause button (TM-05): pauses from play or fast forward, plays from paused. False when
    // nothing changed (the battle has ended).
    bool press_pause(std::uint64_t next_tick);
    // The fast-forward button (TM-06): on from play, off from fast forward, nothing while paused
    // or after the end. False when nothing changed.
    bool press_fast_forward(std::uint64_t next_tick);
    // The pause banner's Resume Game button (TM-09): plays when paused.
    bool resume(std::uint64_t next_tick);
    // #453 BEP-02: the battle ended at `tick`; the panel stops and no tick runs any more.
    void end(std::uint64_t tick);

    [[nodiscard]] TimeState state() const noexcept { return state_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }
    [[nodiscard]] bool paused() const noexcept { return state_ == TimeState::paused; }
    // Whether ticks may run: neither paused nor ended.
    [[nodiscard]] bool running() const noexcept { return !paused() && !ended_; }
    [[nodiscard]] std::uint32_t speed_step() const noexcept { return speed_step_; }
    // The target in logical frames per wall-clock second while running (TM-01, TM-03).
    [[nodiscard]] std::uint32_t target_rate() const noexcept;
    // Presented ticks per nominal game second's worth of frames: target / 30 while running, 0
    // otherwise (TP-06).
    [[nodiscard]] double tick_factor() const noexcept;
    // TM-08: the fast-forward button is disabled while paused (and after the end); the pause
    // button after the end only.
    [[nodiscard]] bool fast_forward_enabled() const noexcept { return !paused() && !ended_; }
    [[nodiscard]] bool pause_enabled() const noexcept { return !ended_; }
    [[nodiscard]] const std::vector<TimeChange>& track() const noexcept { return track_; }
    // "tick,state,ticks_per_second" and one row per change (TP-04).
    [[nodiscard]] std::string track_csv() const;

private:
    void record(std::uint64_t tick, std::string cause);

    std::uint32_t speed_step_;
    TimeState state_{TimeState::play};
    bool ended_{};
    std::vector<TimeChange> track_;
};

} // namespace eawr::presentation::ui
