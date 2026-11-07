#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

// Engine-free pieces of the opt-in additive merge: several emitters' streams are joined into one
// surface by laying their vertices end to end and shifting each stream's indices by the number of
// vertices before it.

// Appends `indices` shifted by `offset`; the stream's own order is kept.
inline void append_rebased_indices(const std::span<const std::uint32_t> indices, const std::uint32_t offset,
                                   std::vector<std::uint32_t>& out) {
    for (const std::uint32_t index : indices) out.push_back(index + offset);
}

// Writes `topology` into a zeroed index buffer of `capacity` indices of `stride` (2 or 4) bytes.
// Slots past the live indices stay zero: degenerate triangles, as the per-emitter surfaces do.
inline void write_index_bytes(const std::span<const std::uint32_t> topology, const std::size_t stride,
                              const std::size_t capacity, std::uint8_t* output) {
    std::memset(output, 0, capacity * stride);
    for (std::size_t index = 0; index < topology.size() && index < capacity; ++index) {
        if (stride == sizeof(std::uint16_t)) {
            const auto value = static_cast<std::uint16_t>(topology[index]);
            std::memcpy(output + index * stride, &value, sizeof(value));
        } else {
            std::memcpy(output + index * stride, &topology[index], sizeof(std::uint32_t));
        }
    }
}

// `--eawr-particle-merge-additive [on|off]`: the merge is on by default; the flag turns it off only
// with the value "off". A bare flag, "on" or any other next argument leaves it on (the next
// argument is another option then, not a value).
[[nodiscard]] inline bool merge_additive_from_argument(const std::string_view next_argument) {
    return next_argument != "off";
}

} // namespace eawr::presentation::godot_backend::detail
