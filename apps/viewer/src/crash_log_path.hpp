#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace eawr::presentation::godot_backend {

// Godot consumes --log-file before exposing engine arguments to extensions.
// Launchers using that override mirror it after '--' as this user argument.
inline std::string_view select_crash_log_path(const std::string_view current,
                                            const std::string_view argument) noexcept {
    constexpr std::string_view prefix = "--eawr-log-file=";
    if (argument.starts_with(prefix) && argument.size() > prefix.size()) return argument.substr(prefix.size());
    return current;
}

// Healthy startup only: centralize the no-user-argument/default-path case so
// the engine-free probe exercises the same selection as scene initialization.
template <typename ArgumentAt>
std::string resolve_crash_log_path(const std::string_view default_log, const std::size_t count,
                                   ArgumentAt argument_at) {
    std::string selected(default_log);
    for (std::size_t index = 0; index < count; ++index) {
        const auto argument = argument_at(index);
        selected = std::string(select_crash_log_path(selected, argument));
    }
    return selected;
}

} // namespace eawr::presentation::godot_backend
