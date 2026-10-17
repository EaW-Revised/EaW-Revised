#pragma once

#include <cmath>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace eawr::viewer::galactic {
// WGUI-68: authored seconds per character. A missing/invalid value is a named
// silent-text seam; it cannot refuse the HUD or the independent approach SFX.
[[nodiscard]] inline std::optional<double> advisor_hint_duration(std::string_view authored) {
    std::istringstream stream{std::string(authored)};
    double value{};
    if (!(stream >> value) || !std::isfinite(value) || value < 0) return std::nullopt;
    stream >> std::ws;
    if (!stream.eof()) return std::nullopt;
    return value;
}
} // namespace eawr::viewer::galactic
