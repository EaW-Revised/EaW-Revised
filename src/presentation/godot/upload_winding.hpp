#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace eawr::presentation::godot_backend::detail {

// ALO's outward triangle order is CCW about its stored normal. Godot's
// cull_back front face is clockwise, so swap the final two indices for every
// triangle at the upload boundary. This applies to all material families;
// cull_disabled still draws both sides, and cull_front keeps the opposite side.
[[nodiscard]] inline std::uint16_t uploaded_triangle_index(
    const std::span<const std::uint16_t> source, const std::size_t position) noexcept {
    const std::size_t triangle = position - position % 3;
    // Indices after the last complete triangle are passed through unchanged.
    if (triangle + 2 >= source.size()) return source[position];
    const std::size_t corner = position % 3;
    return source[triangle + (corner == 1 ? 2 : corner == 2 ? 1 : 0)];
}

} // namespace eawr::presentation::godot_backend::detail
