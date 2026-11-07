#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::presentation::godot_backend {

// Engine-free parsing of the `--eawr-depth-prepass on|off` user argument (also `=on` / `=off`).
// The engine reads rendering/driver/depth_prepass/enable once when the renderer is created, so the
// extension applies the result at SERVERS initialization and nothing runs per frame.
struct DepthPrepassOption {
    std::optional<bool> value;  // absent: the project setting decides
    std::string error;          // non-empty: a malformed or missing value; the caller reports it loudly
};

// Only the exact spellings on and off. The last well-formed occurrence wins, like the other --eawr-* options,
// but every malformed occurrence is reported (the first message is kept); a malformed value is never applied.
template <typename ArgumentAt>
DepthPrepassOption select_depth_prepass(const std::size_t count, ArgumentAt argument_at) {
    constexpr std::string_view flag = "--eawr-depth-prepass";
    DepthPrepassOption result;
    const auto take = [&](const std::string_view text) {
        if (text == "on") result.value = true;
        else if (text == "off") result.value = false;
        else if (result.error.empty())
            result.error = "--eawr-depth-prepass expects on or off, got '" + std::string(text) + "'";
    };
    for (std::size_t index = 0; index < count; ++index) {
        const std::string argument = argument_at(index);
        if (argument == flag) {
            if (index + 1 < count) take(std::string(argument_at(index + 1)));
            else if (result.error.empty()) result.error = "--eawr-depth-prepass is missing its value (on or off)";
        } else if (argument.starts_with(flag) && argument.size() > flag.size() && argument[flag.size()] == '=') {
            take(std::string_view(argument).substr(flag.size() + 1));
        }
    }
    return result;
}

} // namespace eawr::presentation::godot_backend
