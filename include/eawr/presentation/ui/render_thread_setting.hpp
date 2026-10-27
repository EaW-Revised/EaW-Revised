#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace eawr::presentation::ui {

// The text form starts with "eawr-settings <version>"; unsupported versions use defaults.
inline constexpr std::uint32_t settings_version = 1;
enum class RenderThreadSetting : int { separate, safe };

// The native launcher reads this one startup-only choice without linking the
// viewer libraries. Keep its header/version and numeric rules shared with UI settings.
[[nodiscard]] inline std::optional<RenderThreadSetting> stored_render_thread(std::string_view text) {
    const auto trim = [](std::string_view value) {
        const auto last = value.find_last_not_of(" \t\r");
        value = value.substr(0, last == std::string_view::npos ? 0 : last + 1);
        const auto first = value.find_first_not_of(" \t");
        if (first == std::string_view::npos) return std::string_view{};
        return value.substr(first);
    };
    const auto number = [](const std::string_view value) -> std::optional<int> {
        if (value.empty()) return std::nullopt;
        int result{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
        return result;
    };
    bool header = false;
    std::optional<RenderThreadSetting> result;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == '#') continue;
        if (!header) {
            constexpr std::string_view prefix = "eawr-settings ";
            if (!line.starts_with(prefix)) return std::nullopt;
            const auto version = number(trim(line.substr(prefix.size())));
            if (!version || static_cast<std::uint32_t>(*version) != settings_version) return std::nullopt;
            header = true;
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos || trim(line.substr(0, equals)) != "video.render_thread") continue;
        if (const auto value = number(trim(line.substr(equals + 1))))
            result = *value <= 0 ? RenderThreadSetting::separate : RenderThreadSetting::safe;
    }
    return result;
}

} // namespace eawr::presentation::ui
