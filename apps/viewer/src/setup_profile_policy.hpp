#pragma once

namespace eawr::presentation::godot_backend {

// A launcher report is normal in the interactive front end; test and capture runs stay isolated.
[[nodiscard]] constexpr bool setup_uses_player_profile(bool named, bool live_front_end, bool report,
                                                        bool capture, bool synthetic) {
    return named || (!synthetic && !capture && (!report || live_front_end));
}

} // namespace eawr::presentation::godot_backend
