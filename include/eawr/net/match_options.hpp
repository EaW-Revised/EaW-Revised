#pragma once

#include "eawr/net/galactic_clock_policy.hpp"
#include "eawr/net/lockstep_session.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <string_view>

namespace eawr::net {

// MP-06: both mode choices are 32-bit values. This is an opaque sealed input,
// not a transport message kind; the enclosing batch authenticates its seat/key.
inline constexpr std::string_view match_options_domain = "EAWR-MATCH-OPTIONS-1";
inline constexpr std::uint32_t match_speed_choices = 5; // MP-06
// MP-06 / MP-U1: local speed requests share ClockThrottle's budget (#4574).
// Ticks are session ticks (30 per session second), as in ClockThrottle. An accepted tick expires once its age
// exceeds the window.
class MatchSpeedWindow {
public:
    bool accept(std::uint64_t session_tick) {
        while (!accepted_.empty() && session_tick >= accepted_.front() && session_tick - accepted_.front() > ClockThrottle::window_ticks)
            accepted_.pop_front();
        if (accepted_.size() >= ClockThrottle::speed_changes_per_window) return false;
        accepted_.push_back(session_tick);
        return true;
    }
private:
    std::deque<std::uint64_t> accepted_;
};
struct MatchOptions {
    std::uint32_t tactical_speed{2}; // MP-02 default
    std::uint32_t strategic_speed{2};
    friend bool operator==(const MatchOptions&, const MatchOptions&) = default;
};
[[nodiscard]] inline bool is_match_options(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= match_options_domain.size()
        && std::equal(match_options_domain.begin(), match_options_domain.end(), bytes.begin());
}
[[nodiscard]] inline core::Diagnostic match_options_error(std::string_view code, std::string_view message) {
    core::Diagnostic error;
    error.code = code;
    error.message = message;
    return error;
}
[[nodiscard]] inline core::Result<MatchOptions> decode_match_options(std::span<const std::uint8_t> bytes) {
    if (!is_match_options(bytes) || bytes.size() != match_options_domain.size() + 8)
        return core::Result<MatchOptions>::failure(match_options_error("EAWR-NET-1400", "Malformed match options input."));
    std::array<std::uint32_t, 2> choices{};
    for (std::size_t index = 0; index < choices.size(); ++index)
        for (unsigned byte = 0; byte < 4; ++byte)
            choices[index] |= static_cast<std::uint32_t>(bytes[match_options_domain.size() + index * 4 + byte]) << (byte * 8);
    if (choices[0] >= match_speed_choices || choices[1] >= match_speed_choices)
        return core::Result<MatchOptions>::failure(match_options_error("EAWR-NET-1401", "Match speed choice is outside MP-06's range."));
    return core::Result<MatchOptions>::success({choices[0], choices[1]});
}
[[nodiscard]] inline core::Result<std::vector<std::uint8_t>> encode_match_options(MatchOptions options) {
    std::vector<std::uint8_t> bytes(match_options_domain.begin(), match_options_domain.end());
    for (const auto choice : {options.tactical_speed, options.strategic_speed})
        for (unsigned byte = 0; byte < 4; ++byte) bytes.push_back(static_cast<std::uint8_t>(choice >> (byte * 8)));
    auto decoded = decode_match_options(bytes);
    if (!decoded) return core::Result<std::vector<std::uint8_t>>::failure(decoded.error());
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}
} // namespace eawr::net
