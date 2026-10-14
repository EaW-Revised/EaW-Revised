#pragma once

namespace eawr::core {

// Authoritative identifiers and token separators are ASCII, independent of the process locale.
[[nodiscard]] constexpr unsigned char ascii_lower(unsigned char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<unsigned char>(value + ('a' - 'A')) : value;
}
[[nodiscard]] constexpr unsigned char ascii_upper(unsigned char value) noexcept {
    return value >= 'a' && value <= 'z' ? static_cast<unsigned char>(value - ('a' - 'A')) : value;
}
[[nodiscard]] constexpr bool ascii_space(unsigned char value) noexcept {
    return value == ' ' || (value >= '\t' && value <= '\r');
}
[[nodiscard]] constexpr bool ascii_digit(unsigned char value) noexcept {
    return value >= '0' && value <= '9';
}
[[nodiscard]] constexpr bool ascii_alpha(unsigned char value) noexcept {
    const auto lower = ascii_lower(value);
    return lower >= 'a' && lower <= 'z';
}
[[nodiscard]] constexpr bool ascii_alnum(unsigned char value) noexcept {
    return ascii_alpha(value) || ascii_digit(value);
}

} // namespace eawr::core
