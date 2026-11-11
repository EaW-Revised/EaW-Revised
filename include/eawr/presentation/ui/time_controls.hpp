#pragma once

// #459: the tactical time panel (docs/behaviour/tactical-time-controls.md). The pause and
// fast-forward buttons and the speed setting decide how many logical frames run per
// wall-clock second, or none; nothing here changes what a tick does (TP-02). The model is
// presentation state: it never enters a command, the replay or a hash. Each change is
// recorded in the time track (TP-04) at the tick the next step would run.

#include <array>
#include <cstddef>
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

// #5355 (TP-MP06): a LAN Galactic Conquest battle child shares one tactical speed through the agreed speed command.
// Its fast-forward button requests the multiplayer maximum, 60 ticks a second (MP-06's setting 4, 2x), and back to
// the nominal 30; the 120 of TM-03 is never requested there.
inline constexpr std::uint32_t shared_fast_forward_rate = 60;
[[nodiscard]] constexpr std::uint32_t shared_fast_forward_target(const std::uint32_t agreed_rate) noexcept {
    return agreed_rate == shared_fast_forward_rate ? nominal_rate : shared_fast_forward_rate;
}
// One shared speed request in flight: a second press waits until the agreed rate equals the request, or one shared
// speed window (300 ticks) has passed, so a double click cannot spend the 2-per-10 s budget. Presentation only.
class SharedSpeedRequest final {
public:
    static constexpr std::uint64_t release_ticks = 300;
    [[nodiscard]] bool begin(const std::uint32_t agreed_rate, const std::uint32_t target, const std::uint64_t tick) {
        if (pending_ || agreed_rate == target) return false;
        pending_ = Pending{target, tick};
        return true;
    }
    void observe(const std::uint32_t agreed_rate, const std::uint64_t tick) {
        if (pending_ && (agreed_rate == pending_->target || tick < pending_->since || tick - pending_->since >= release_ticks))
            pending_.reset();
    }
    void reset() noexcept { pending_.reset(); }
    [[nodiscard]] std::optional<std::uint32_t> target() const noexcept {
        return pending_ ? std::optional{pending_->target} : std::nullopt;
    }
private:
    struct Pending { std::uint32_t target{}; std::uint64_t since{}; };
    std::optional<Pending> pending_;
};

// #5355 (TP-MP06): a target rate as the players read it against the nominal 30 ("2x", "1.5x", "0.33x").
[[nodiscard]] std::string speed_factor_label(double ticks_per_second);
// What the shared speed line says, as a small comparable value, so a HUD formats text only when it changes. Lockstep
// runs at the slowest seat and never skips a tick (MP-31), so the requested rate is a request: at the nominal 30 there
// is no line; otherwise it names the request, and the measured rate (in tenths of 1x) when that is below 95% of it.
struct SharedSpeedReading {
    enum class Pace : std::uint8_t { none, measuring, held, slower };
    std::uint32_t requested_rate{};
    Pace pace{Pace::none};
    std::int64_t running_tenths{}; // `slower` only
    [[nodiscard]] bool operator==(const SharedSpeedReading&) const = default;
};
[[nodiscard]] SharedSpeedReading read_shared_speed(std::uint32_t requested_rate, std::optional<double> ticks_per_second) noexcept;
// The shared speed line of a multiplayer battle (`clock` names it, "Battle"). Empty at 30.
[[nodiscard]] std::string shared_speed_line(std::string_view clock, const SharedSpeedReading& reading);
[[nodiscard]] inline std::string shared_speed_line(const std::string_view clock, const std::uint32_t requested_rate,
    const std::optional<double> ticks_per_second) {
    return shared_speed_line(clock, read_shared_speed(requested_rate, ticks_per_second));
}
// What really runs: agreed ticks against the presentation clock over a short window. At most one sample per sample_ms
// goes into fixed storage, so the work and memory do not depend on how often the HUD refreshes and nothing allocates.
// A new requested rate, a pause, a gap in the samples or a tick going back starts a new measurement. Presentation only.
class SharedPaceMeter final {
public:
    static constexpr std::uint64_t window_ms = 3000;
    static constexpr std::uint64_t min_span_ms = 1000;
    static constexpr std::uint64_t max_gap_ms = 1000;
    static constexpr std::uint64_t sample_ms = 100;
    static constexpr std::size_t capacity = window_ms / sample_ms + 2;
    void sample(std::uint64_t tick, std::uint64_t now_ms, std::uint32_t requested_rate, bool running) noexcept;
    [[nodiscard]] std::optional<double> ticks_per_second() const noexcept;
    [[nodiscard]] std::size_t samples() const noexcept { return count_; }
private:
    struct Sample { std::uint64_t tick{}, ms{}; };
    [[nodiscard]] const Sample& at(const std::size_t index) const noexcept { return ring_[(first_ + index) % capacity]; }
    void drop_first() noexcept { first_ = (first_ + 1) % capacity; --count_; }
    std::array<Sample, capacity> ring_{};
    std::size_t first_{}, count_{};
    std::uint32_t requested_rate_{};
};
// The HUD notice of a multiplayer battle's shared speed: who changed it (for actor_ms of presentation time) and, while
// the requested rate is not 30, the shared speed line. refresh() says whether the text changed; an unchanged speed,
// actor state and measured pace cost no formatting and no allocation. Presentation only.
class SharedSpeedNotice final {
public:
    static constexpr std::uint64_t actor_ms = 5000;
    // The speed event identity last taken; a caller formats an actor line only when its own identity differs from it.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    // A new speed event: `actor_line` ("<name> set the battle speed to 2x"; empty for a clock event that names nobody)
    // shows from `now_ms` on.
    void change(std::uint64_t revision, std::string actor_line, std::uint64_t now_ms);
    [[nodiscard]] bool refresh(std::uint64_t tick, std::uint64_t now_ms, std::uint32_t requested_rate, bool advancing);
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
private:
    SharedPaceMeter pace_;
    std::string actor_, text_;
    std::uint64_t revision_{}, actor_since_ms_{};
    bool actor_shown_{}, refreshed_{};
    SharedSpeedReading reading_{};
};

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
    // MP-06: presentation observes an already agreed network speed choice.
    bool set_speed_step(std::uint32_t step, std::uint64_t next_tick);
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
