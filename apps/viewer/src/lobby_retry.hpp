#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace eawr::presentation::godot_backend {

// Acceptance automation retries a transient refusal without flooding a pending
// request. Each lobby revision gets at most three attempts, five seconds apart.
class LobbyRetry {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] bool attempt(std::uint64_t revision, Clock::time_point now) {
        if (revision_ != revision) {
            revision_ = revision;
            attempts_ = 0;
            next_ = {};
        }
        if (attempts_ == 3 || now < next_) return false;
        ++attempts_;
        next_ = now + std::chrono::seconds(5);
        return true;
    }

private:
    std::optional<std::uint64_t> revision_;
    unsigned attempts_{};
    Clock::time_point next_{};
};

} // namespace eawr::presentation::godot_backend
