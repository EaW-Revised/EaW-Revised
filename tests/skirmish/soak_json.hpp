#pragma once

#include "eawr/core/json_text.hpp"

// Just enough JSON for the soak's summaries (#627): a quoted string and a finite number.

#include <cmath>
#include <cstdio>
#include <string>

namespace eawr::soak {

[[nodiscard]] inline std::string json_string(const std::string& text) {
    return eawr::core::json_quote(text);
}

// Six decimals; a value that is not finite (never, by construction) is written as 0.
[[nodiscard]] inline std::string json_number(const double value) {
    if (!std::isfinite(value)) return "0";
    char text[64];
    std::snprintf(text, sizeof text, "%.6f", value);
    return text;
}

} // namespace eawr::soak
