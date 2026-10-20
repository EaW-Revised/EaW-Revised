#pragma once

#include "eawr/core/ascii.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace eawr::core {

// Keep each format's separators explicit. Bytes outside ASCII pass through.
inline constexpr std::string_view ascii_whitespace = " \t\r\n\f\v";
inline constexpr std::string_view xml_whitespace = " \t\r\n";
inline constexpr std::string_view horizontal_whitespace = " \t";
inline constexpr std::string_view line_whitespace = " \t\r";

[[nodiscard]] constexpr std::string_view trim_view(std::string_view text,
    const std::string_view whitespace = ascii_whitespace) noexcept {
    while (!text.empty() && whitespace.find(text.front()) != std::string_view::npos) text.remove_prefix(1);
    while (!text.empty() && whitespace.find(text.back()) != std::string_view::npos) text.remove_suffix(1);
    return text;
}

[[nodiscard]] inline std::string trim(const std::string_view text,
    const std::string_view whitespace = ascii_whitespace) {
    return std::string(trim_view(text, whitespace));
}

[[nodiscard]] inline std::string to_lower(const std::string_view text) {
    std::string result(text);
    for (char& value : result) value = static_cast<char>(ascii_lower(static_cast<unsigned char>(value)));
    return result;
}

[[nodiscard]] inline std::string to_upper(const std::string_view text) {
    std::string result(text);
    for (char& value : result) value = static_cast<char>(ascii_upper(static_cast<unsigned char>(value)));
    return result;
}

[[nodiscard]] constexpr bool iequals(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(static_cast<unsigned char>(left[index])) !=
            ascii_lower(static_cast<unsigned char>(right[index]))) return false;
    }
    return true;
}

} // namespace eawr::core
