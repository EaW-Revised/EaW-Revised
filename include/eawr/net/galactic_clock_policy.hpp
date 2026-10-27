#pragma once

#include "eawr/sim/galactic/types.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace eawr::net {

// D7 is a project LAN option, independent of retail single-player clock rules.
enum class GalacticClockPolicy : std::uint8_t { unrestricted, limited };

// #4574: the one place the shared-clock limits live. Pause and resume never use the speed budget; they only have a
// short per-seat debounce against double clicks. Every duration is in session seconds (30 ticks per second).
struct ClockThrottle {
    static constexpr std::uint64_t window_seconds = 45;
    static constexpr std::size_t speed_changes_per_window = 6;
    static constexpr std::uint64_t pause_debounce_ticks = sim::galactic::galactic_frames_per_second; // 1 s
    static constexpr std::uint64_t window_ticks = window_seconds * sim::galactic::galactic_frames_per_second;
    static constexpr std::uint64_t window_ms = window_seconds * 1000;
};

class GalacticClockLimiter final {
public:
    static constexpr std::uint64_t window_ticks = ClockThrottle::window_ticks;
    static constexpr std::size_t changes_per_window = ClockThrottle::speed_changes_per_window;
    using History = std::map<sim::SeatId, std::deque<std::uint64_t>>;
    using Toggles = std::map<sim::SeatId, std::uint64_t>;

    explicit GalacticClockLimiter(GalacticClockPolicy policy = GalacticClockPolicy::unrestricted) : policy_(policy) {}
    void expire(std::uint64_t tick) {
        for (auto at = history_.begin(); at != history_.end();) {
            auto& changes = at->second;
            while (!changes.empty() && tick >= changes.front() && tick - changes.front() >= window_ticks)
                changes.pop_front();
            if (changes.empty()) at = history_.erase(at);
            else ++at;
        }
        for (auto at = toggles_.begin(); at != toggles_.end();) {
            if (tick >= at->second && tick - at->second >= ClockThrottle::pause_debounce_ticks) at = toggles_.erase(at);
            else ++at;
        }
    }
    // Called only at the executing sealed tick, in canonical command order.
    [[nodiscard]] bool admit_speed(sim::SeatId seat, std::uint64_t tick) {
        if (policy_ == GalacticClockPolicy::unrestricted) return true;
        expire(tick);
        auto& changes = history_[seat];
        if (changes.size() >= changes_per_window) return false;
        changes.push_back(tick);
        return true;
    }
    // A pause is refused only inside the debounce after this seat's previous pause or resume.
    [[nodiscard]] bool admit_pause(sim::SeatId seat, std::uint64_t tick) {
        if (policy_ == GalacticClockPolicy::unrestricted) return true;
        expire(tick);
        if (toggles_.contains(seat)) return false;
        toggles_[seat] = tick;
        return true;
    }
    // Resume is never refused; it only restarts the seat's debounce.
    void note_resume(sim::SeatId seat, std::uint64_t tick) {
        if (policy_ == GalacticClockPolicy::unrestricted) return;
        expire(tick);
        toggles_[seat] = tick;
    }
    // Whole session seconds until the seat's next speed change is allowed (0 when one is allowed now).
    [[nodiscard]] std::uint64_t speed_wait_seconds(sim::SeatId seat, std::uint64_t tick) const {
        const auto at = history_.find(seat);
        if (at == history_.end() || at->second.size() < changes_per_window) return 0;
        const auto ready = at->second.front() + window_ticks;
        const auto ticks = ready > tick ? ready - tick : 0;
        return (ticks + sim::galactic::galactic_frames_per_second - 1) / sim::galactic::galactic_frames_per_second;
    }
    [[nodiscard]] GalacticClockPolicy policy() const noexcept { return policy_; }
    [[nodiscard]] const History& history() const noexcept { return history_; }
    [[nodiscard]] const Toggles& toggles() const noexcept { return toggles_; }
private:
    GalacticClockPolicy policy_;
    History history_;
    Toggles toggles_;
};

} // namespace eawr::net
