#pragma once

// Just enough JSON for the soak's summaries (#627): a quoted string and a finite number.

#include <cmath>
#include <cstdio>
#include <string>

namespace eawr::soak {

[[nodiscard]] inline std::string json_string(const std::string& text) {
    std::string quoted = "\"";
    for (const char letter : text) {
        switch (letter) {
        case '"': quoted += "\\\""; break;
        case '\\': quoted += "\\\\"; break;
        case '\n': quoted += "\\n"; break;
        case '\r': quoted += "\\r"; break;
        case '\t': quoted += "\\t"; break;
        default:
            if (static_cast<unsigned char>(letter) < 0x20) {
                char escape[8];
                std::snprintf(escape, sizeof escape, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(letter)));
                quoted += escape;
            } else {
                quoted += letter;
            }
        }
    }
    return quoted + '"';
}

// Six decimals; a value that is not finite (never, by construction) is written as 0.
[[nodiscard]] inline std::string json_number(const double value) {
    if (!std::isfinite(value)) return "0";
    char text[64];
    std::snprintf(text, sizeof text, "%.6f", value);
    return text;
}

} // namespace eawr::soak
