#pragma once

#include <chrono>

namespace eawr::net {
// All polls, including a sender's echo, share one spacing gate. A send cannot
// add a second poll just after a periodic one and exhaust a shared relay budget.
class MenuChatPollSchedule {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto send_interval = std::chrono::milliseconds(1100);
    static constexpr auto focused_interval = std::chrono::milliseconds(1100);
    static constexpr auto background_interval = std::chrono::milliseconds(2000);

    void set_focused(bool focused, Clock::time_point now) {
        if (focused_ == focused) return;
        focused_ = focused;
        next_ = now + interval();
    }
    void received(Clock::time_point now) {
        last_ = now;
        next_ = now + interval();
    }
    [[nodiscard]] bool due(Clock::time_point now, bool sent) const {
        return now >= next_ || (sent && now - last_ >= focused_interval);
    }
private:
    [[nodiscard]] std::chrono::milliseconds interval() const {
        return focused_ ? focused_interval : background_interval;
    }
    bool focused_{};
    Clock::time_point last_{}, next_{};
};
} // namespace eawr::net
