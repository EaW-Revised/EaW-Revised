#pragma once
#include "eawr/core/ascii.hpp"
#include "eawr/core/text.hpp"

#include <string_view>

namespace eawr::data::ui::detail {

[[nodiscard]] constexpr char ascii_lower(const char ch) noexcept {
    return static_cast<char>(core::ascii_lower(static_cast<unsigned char>(ch)));
}

// ASCII case-insensitive equality without allocation.
[[nodiscard]] constexpr bool iequals(const std::string_view left, const std::string_view right) noexcept {
    return core::iequals(left, right);
}

} // namespace eawr::data::ui::detail
