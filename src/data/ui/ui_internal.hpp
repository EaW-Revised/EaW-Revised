#pragma once

#include <algorithm>
#include <string_view>

namespace eawr::data::ui::detail {

[[nodiscard]] constexpr char ascii_lower(const char ch) noexcept {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

// ASCII case-insensitive equality without allocation.
[[nodiscard]] constexpr bool iequals(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](const char a, const char b) { return ascii_lower(a) == ascii_lower(b); });
}

} // namespace eawr::data::ui::detail
