#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace eawr::net {
// Link liveness, not a per-message ACK counter. A steady in-flight window can
// replace acknowledged bytes with new sends between samples. Retransmission can
// also move bytes from unacknowledged to pending without acknowledging anything.
class GnsLinkWatchdog {
public:
    using Clock = std::chrono::steady_clock;
    void received_reliable(Clock::time_point now) {
        if (silent_since_) silent_since_ = now;
    }
    std::chrono::milliseconds observe(Clock::time_point now, std::int32_t pending, std::int32_t unacked) {
        const auto outstanding = static_cast<std::int64_t>(std::max(0, pending)) + std::max(0, unacked);
        if (outstanding == 0 || outstanding < previous_) silent_since_.reset();
        else if (!silent_since_ && unacked > 0) silent_since_ = now;
        previous_ = outstanding;
        return silent_since_ ? std::chrono::duration_cast<std::chrono::milliseconds>(now - *silent_since_)
            : std::chrono::milliseconds{};
    }
    void reset() { silent_since_.reset(); previous_ = 0; }
private:
    std::optional<Clock::time_point> silent_since_;
    std::int64_t previous_{};
};
} // namespace eawr::net
