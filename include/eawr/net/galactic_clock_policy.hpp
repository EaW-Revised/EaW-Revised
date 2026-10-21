#pragma once

#include "eawr/sim/galactic/types.hpp"
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>

namespace eawr::net {

// D7 is a project LAN option, independent of retail single-player clock rules.
enum class GalacticClockPolicy : std::uint8_t { unrestricted, two_per_45_seconds };

class GalacticClockLimiter final {
public:
    static constexpr std::uint64_t window_ticks = 45 * sim::galactic::galactic_frames_per_second;
    static constexpr std::size_t changes_per_window = 2;
    using History = std::map<sim::SeatId, std::deque<std::uint64_t>>;

    explicit GalacticClockLimiter(GalacticClockPolicy policy = GalacticClockPolicy::unrestricted) : policy_(policy) {}
    void expire(std::uint64_t tick) {
        for (auto at = history_.begin(); at != history_.end();) {
            auto& changes = at->second;
            while (!changes.empty() && tick >= changes.front() && tick - changes.front() >= window_ticks)
                changes.pop_front();
            if (changes.empty()) at = history_.erase(at);
            else ++at;
        }
    }
    // Called only at the executing sealed tick, in canonical command order.
    [[nodiscard]] bool admit(sim::SeatId seat, std::uint64_t tick) {
        if (policy_ == GalacticClockPolicy::unrestricted) return true;
        expire(tick);
        auto& changes = history_[seat];
        if (changes.size() >= changes_per_window) return false;
        changes.push_back(tick);
        return true;
    }
    [[nodiscard]] GalacticClockPolicy policy() const noexcept { return policy_; }
    [[nodiscard]] const History& history() const noexcept { return history_; }
private:
    GalacticClockPolicy policy_;
    History history_;
};

} // namespace eawr::net
