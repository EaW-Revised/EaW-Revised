#include "eawr/presentation/ui/time_controls.hpp"

#include <algorithm>

namespace eawr::presentation::ui {

std::string_view to_string(const TimeState state) noexcept {
    switch (state) {
    case TimeState::play: return "play";
    case TimeState::paused: return "paused";
    case TimeState::fast_forward: return "fast_forward";
    }
    return "play";
}

TimeControls::TimeControls(const std::uint32_t speed_step)
    : speed_step_(std::min<std::uint32_t>(speed_step, static_cast<std::uint32_t>(speed_step_rates.size() - 1U))) {}

bool TimeControls::press_pause(const std::uint64_t next_tick) {
    if (ended_) return false;
    // TM-05: pausing from fast forward ends it; playing returns to the speed setting.
    state_ = paused() ? TimeState::play : TimeState::paused;
    record(next_tick, paused() ? "pause" : "play");
    return true;
}

bool TimeControls::press_fast_forward(const std::uint64_t next_tick) {
    // TM-06: nothing while paused.
    if (ended_ || paused()) return false;
    state_ = state_ == TimeState::fast_forward ? TimeState::play : TimeState::fast_forward;
    record(next_tick, state_ == TimeState::fast_forward ? "fast_forward" : "normal_speed");
    return true;
}

bool TimeControls::resume(const std::uint64_t next_tick) {
    if (ended_ || !paused()) return false;
    state_ = TimeState::play;
    record(next_tick, "resume");
    return true;
}

void TimeControls::end(const std::uint64_t tick) {
    if (ended_) return;
    ended_ = true;
    record(tick, "end");
}

std::uint32_t TimeControls::target_rate() const noexcept {
    return state_ == TimeState::fast_forward ? fast_forward_rate : speed_step_rates[speed_step_];
}

double TimeControls::tick_factor() const noexcept {
    if (!running()) return 0.0;
    return static_cast<double>(target_rate()) / static_cast<double>(nominal_rate);
}

void TimeControls::record(const std::uint64_t tick, std::string cause) {
    track_.push_back({tick, state_, running() ? target_rate() : 0U, std::move(cause)});
}

std::string TimeControls::track_csv() const {
    std::string output = "tick,state,ticks_per_second\n";
    for (const TimeChange& change : track_) {
        const std::string_view state = change.cause == "end" ? std::string_view("ended") : to_string(change.state);
        output += std::to_string(change.tick) + ',' + std::string(state) + ',' + std::to_string(change.target_rate) + '\n';
    }
    return output;
}

} // namespace eawr::presentation::ui
