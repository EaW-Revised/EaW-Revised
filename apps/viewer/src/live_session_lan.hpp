#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace eawr::presentation::godot_backend::live_session {

// Presentation-thread timing only; the match and feed retain their own lifetimes.
struct LanTiming final {
    std::optional<std::uint64_t> end_frame;
    std::optional<std::chrono::steady_clock::time_point> progress_at;
};

} // namespace eawr::presentation::godot_backend::live_session
