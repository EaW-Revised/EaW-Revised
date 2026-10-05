#pragma once

#include "map_mode_internal.hpp"

namespace eawr::presentation::godot_backend {

template <class T> [[nodiscard]] std::optional<T> parse_unsigned(const std::string_view text) {
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return result;
}


} // namespace eawr::presentation::godot_backend
