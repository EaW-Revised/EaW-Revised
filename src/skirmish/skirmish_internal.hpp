#pragma once

#include "eawr/skirmish/start.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::skirmish::detail {

[[nodiscard]] core::Diagnostic error(std::string_view code, std::string message);

[[nodiscard]] std::string trim(std::string_view value);
[[nodiscard]] bool iequals(std::string_view left, std::string_view right) noexcept;
// Split on commas and whitespace, empties dropped.
[[nodiscard]] std::vector<std::string> tokens(std::string_view text);
// yes/true/1 and no/false/0 in any case; nullopt otherwise.
[[nodiscard]] std::optional<bool> boolean(std::string_view text);
// A boolean tag as retail reads it: true when the value is "1" or starts with Y or T in
// either case, false otherwise; an empty value or TBD leaves `fallback` (AU-80 to AU-82).
[[nodiscard]] bool retail_flag(std::string_view text, bool fallback);
// An authored decimal (Fixed::from_decimal after trimming); nullopt when empty or malformed.
[[nodiscard]] std::optional<Fixed> number(std::string_view text);
// Three or four integers in 0..255 separated by commas and/or whitespace.
[[nodiscard]] std::optional<std::array<std::uint8_t, 3>> colour(std::string_view text);

} // namespace eawr::skirmish::detail
