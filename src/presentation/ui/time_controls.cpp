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

bool TimeControls::set_speed_step(const std::uint32_t step, const std::uint64_t next_tick) {
    if (ended_ || step >= speed_step_rates.size() || step == speed_step_) return false;
    speed_step_ = step;
    record(next_tick, "speed");
    return true;
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

std::string speed_factor_label(const double ticks_per_second) {
    const auto hundredths = static_cast<long long>(ticks_per_second / static_cast<double>(nominal_rate) * 100.0 + 0.5);
    std::string text = std::to_string(hundredths / 100);
    if (const auto fraction = hundredths % 100; fraction != 0) {
        text += '.';
        text += static_cast<char>('0' + fraction / 10);
        if (fraction % 10 != 0) text += static_cast<char>('0' + fraction % 10);
    }
    return text + "x";
}

SharedSpeedReading read_shared_speed(const std::uint32_t requested_rate, const std::optional<double> ticks_per_second) noexcept {
    using Pace = SharedSpeedReading::Pace;
    if (requested_rate == nominal_rate || requested_rate == 0) return {};
    if (!ticks_per_second) return {requested_rate, Pace::measuring, 0};
    if (*ticks_per_second >= static_cast<double>(requested_rate) * 0.95) return {requested_rate, Pace::held, 0};
    return {requested_rate, Pace::slower, static_cast<std::int64_t>(*ticks_per_second / 3.0 + 0.5)};
}

std::string shared_speed_line(const std::string_view clock, const SharedSpeedReading& reading) {
    using Pace = SharedSpeedReading::Pace;
    if (reading.pace == Pace::none) return {};
    const std::string head = std::string(clock) + " speed " + speed_factor_label(static_cast<double>(reading.requested_rate));
    if (reading.pace == Pace::measuring) return head + " requested; measuring the real pace";
    if (reading.pace == Pace::held) return head;
    const auto running = reading.running_tenths < 1 ? std::string("under 0.1x")
        : speed_factor_label(static_cast<double>(reading.running_tenths) * 3.0);
    return head + " requested, running at " + running + ": the slowest player's computer sets the pace";
}

void SharedPaceMeter::sample(const std::uint64_t tick, const std::uint64_t now_ms, const std::uint32_t requested_rate,
    const bool running) noexcept {
    if (count_ != 0) {
        const auto& last = at(count_ - 1);
        if (!running || requested_rate != requested_rate_ || now_ms < last.ms || now_ms - last.ms > max_gap_ms || tick < last.tick)
            first_ = count_ = 0;
        else if (now_ms - last.ms < sample_ms) return; // at most one sample per sample_ms
    }
    requested_rate_ = requested_rate;
    if (!running) return;
    if (count_ == capacity) drop_first();
    ring_[(first_ + count_) % capacity] = {tick, now_ms};
    ++count_;
    while (count_ > 2 && now_ms - at(1).ms >= window_ms) drop_first();
}

std::optional<double> SharedPaceMeter::ticks_per_second() const noexcept {
    if (count_ < 2) return std::nullopt;
    const auto& first = at(0);
    const auto& last = at(count_ - 1);
    const auto span = last.ms - first.ms;
    if (span < min_span_ms) return std::nullopt;
    return static_cast<double>(last.tick - first.tick) * 1000.0 / static_cast<double>(span);
}

void SharedSpeedNotice::change(const std::uint64_t revision, std::string actor_line, const std::uint64_t now_ms) {
    revision_ = revision;
    if (actor_line.empty()) return;
    actor_ = std::move(actor_line);
    actor_since_ms_ = now_ms;
    refreshed_ = false; // a new actor line always shows, even with the same text as the last one
}

bool SharedSpeedNotice::refresh(const std::uint64_t tick, const std::uint64_t now_ms, const std::uint32_t requested_rate,
    const bool advancing) {
    pace_.sample(tick, now_ms, requested_rate, advancing && requested_rate != nominal_rate);
    const bool actor = !actor_.empty() && now_ms >= actor_since_ms_ && now_ms - actor_since_ms_ < actor_ms;
    const auto reading = advancing ? read_shared_speed(requested_rate, pace_.ticks_per_second()) : SharedSpeedReading{};
    if (refreshed_ && actor == actor_shown_ && reading == reading_) return false;
    refreshed_ = true;
    actor_shown_ = actor;
    reading_ = reading;
    text_.clear();
    if (actor) text_ = actor_;
    if (const auto line = shared_speed_line("Battle", reading); !line.empty()) {
        if (!text_.empty()) text_ += '\n';
        text_ += line;
    }
    return true;
}

} // namespace eawr::presentation::ui
