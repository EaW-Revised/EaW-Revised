#pragma once

#include "eawr/net/galactic_clock_policy.hpp"
#include "eawr/net/lockstep_session.hpp"
#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::net {

// TP-MP01: owner-requested pause extension; these are opaque sealed seat inputs,
// not tactical opcodes. The domain deliberately refuses earlier encodings.
inline constexpr std::string_view match_pause_domain = "eawr-match-pause-v1";
[[nodiscard]] inline bool is_match_pause(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= match_pause_domain.size()
        && std::equal(match_pause_domain.begin(), match_pause_domain.end(), bytes.begin());
}
[[nodiscard]] inline core::Result<bool> decode_match_pause(std::span<const std::uint8_t> bytes) {
    if (!is_match_pause(bytes) || bytes.size() != match_pause_domain.size() + 1 || bytes.back() > 1) {
        core::Diagnostic error;
        error.code = session_errors::malformed; error.message = "Expected eawr-match-pause-v1 control.";
        return core::Result<bool>::failure(std::move(error));
    }
    return core::Result<bool>::success(bytes.back() != 0);
}
[[nodiscard]] inline core::Result<std::vector<std::uint8_t>> encode_match_pause(bool paused) {
    std::vector<std::uint8_t> bytes(match_pause_domain.begin(), match_pause_domain.end());
    bytes.push_back(paused ? 1 : 0);
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

class MatchPauseClock final {
public:
    enum class Change { unchanged, applied, throttled };
    // Execution order and the session clock alone decide. Resume cannot be throttled; a pause only has the debounce.
    [[nodiscard]] Change apply(bool requested, sim::SeatCommandKey key) {
        limiter.expire(key.tick);
        if (requested == paused) return Change::unchanged;
        if (requested) {
            if (!limiter.admit_pause(key.seat, key.tick)) return Change::throttled;
        } else {
            limiter.note_resume(key.seat, key.tick);
        }
        paused = requested;
        if (paused) actor = key.seat;
        changed_at = key.tick;
        return Change::applied;
    }
    [[nodiscard]] std::vector<std::uint8_t> canonical() const {
        std::vector<std::uint8_t> bytes;
        const auto put = [&](std::uint64_t value, unsigned count) {
            for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
        };
        put(completed, 8); put(paused ? 1U : 0U, 1); put(actor ? actor->value : 0, 4);
        put(actor.has_value() ? 1U : 0U, 1); put(changed_at, 8);
        put(limiter.history().size(), 4);
        for (const auto& [seat, changes] : limiter.history()) {
            put(seat.value, 4); put(changes.size(), 4);
            for (const auto tick : changes) put(tick, 8);
        }
        put(limiter.toggles().size(), 4);
        for (const auto& [seat, tick] : limiter.toggles()) { put(seat.value, 4); put(tick, 8); }
        return bytes;
    }
    std::uint64_t completed{}, changed_at{};
    bool paused{};
    std::optional<sim::SeatId> actor;
    GalacticClockLimiter limiter{GalacticClockPolicy::limited};
};

} // namespace eawr::net
