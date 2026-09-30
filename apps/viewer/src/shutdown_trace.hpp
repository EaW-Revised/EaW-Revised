#pragma once

// Timestamps for the viewer's quit path: how long a window close (or the HUD's Quit Game) takes
// to reach the process exit, and which teardown step the time goes to. Each mark prints one line,
// "EAWR shutdown +<ms> ms <step>", counted from the first mark of the run (the close request).
// The rig suite's live-quit test reads them; a player never sees them (the console build only).

#include <chrono>
#include <string>

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace eawr::presentation::godot_backend::shutdown_trace {

// Milliseconds since the first mark; the first call starts the clock.
[[nodiscard]] inline double elapsed_ms() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

inline void mark(const std::string& step) {
    const double at = elapsed_ms();
    godot::UtilityFunctions::print(godot::String(
        ("EAWR shutdown +" + std::to_string(static_cast<long long>(at)) + " ms " + step).c_str()));
}

} // namespace eawr::presentation::godot_backend::shutdown_trace
