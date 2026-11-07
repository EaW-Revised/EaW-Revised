#pragma once

// #3989: the viewer's presentation hints in a skirmish battle save (checkpoint section 0x8000,
// docs/checkpoint-format.md "Skirmish battle saves"). The battle itself, its lobby setup included,
// is in the save's tactical sections; the hints hold only what the view needs to look the same
// after a load: the camera, and a label for the save list. They never decide the simulation, so a
// load whose hints are missing or unreadable keeps the battle and uses the map's own camera.
//
// Encoding: "EAWRSKH" and the version byte 1, a u32 flag word (bit 0: camera present), then, when
// the camera is present, five IEEE-754 binary32 bit patterns as u32 (render target x, y, z, zoom,
// yaw in degrees), then the label as a u32 byte count and UTF-8 bytes (at most 256). All integers
// are little-endian. Any other byte string decodes to nothing.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eawr::viewer {

struct SkirmishSaveCamera final {
    std::array<float, 3> target{};
    float zoom{};
    float yaw_degrees{};
    friend bool operator==(const SkirmishSaveCamera&, const SkirmishSaveCamera&) = default;
};

struct SkirmishSaveHints final {
    std::optional<SkirmishSaveCamera> camera;
    std::string label; // shown in the save list; the map's name
    friend bool operator==(const SkirmishSaveHints&, const SkirmishSaveHints&) = default;
};

inline constexpr std::array<std::uint8_t, 8> skirmish_save_hints_magic{'E', 'A', 'W', 'R', 'S', 'K', 'H', 1};
inline constexpr std::size_t skirmish_save_hints_label_limit = 256;

[[nodiscard]] inline std::vector<std::uint8_t> encode_skirmish_save_hints(const SkirmishSaveHints& hints) {
    std::vector<std::uint8_t> bytes(skirmish_save_hints_magic.begin(), skirmish_save_hints_magic.end());
    const auto put = [&bytes](const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    put(hints.camera ? 1U : 0U);
    if (hints.camera) {
        for (const float value : {hints.camera->target[0], hints.camera->target[1], hints.camera->target[2],
                                  hints.camera->zoom, hints.camera->yaw_degrees}) {
            put(std::bit_cast<std::uint32_t>(value));
        }
    }
    const std::size_t size = std::min(hints.label.size(), skirmish_save_hints_label_limit);
    put(static_cast<std::uint32_t>(size));
    bytes.insert(bytes.end(), hints.label.begin(), hints.label.begin() + static_cast<std::ptrdiff_t>(size));
    return bytes;
}

[[nodiscard]] inline std::optional<SkirmishSaveHints> decode_skirmish_save_hints(const std::vector<std::uint8_t>& bytes) {
    std::size_t at = skirmish_save_hints_magic.size();
    if (bytes.size() < at) return std::nullopt;
    for (std::size_t index = 0; index < at; ++index) {
        if (bytes[index] != skirmish_save_hints_magic[index]) return std::nullopt;
    }
    const auto take = [&](std::uint32_t& value) {
        if (bytes.size() - at < 4U) return false;
        value = 0;
        for (unsigned shift = 0; shift < 32U; shift += 8U) value |= static_cast<std::uint32_t>(bytes[at++]) << shift;
        return true;
    };
    std::uint32_t flags{};
    if (!take(flags) || (flags & ~1U) != 0U) return std::nullopt;
    SkirmishSaveHints hints;
    if ((flags & 1U) != 0U) {
        std::array<float, 5> values{};
        for (float& value : values) {
            std::uint32_t raw{};
            if (!take(raw)) return std::nullopt;
            value = std::bit_cast<float>(raw);
            if (!std::isfinite(value)) return std::nullopt;
        }
        hints.camera = SkirmishSaveCamera{{values[0], values[1], values[2]}, values[3], values[4]};
    }
    std::uint32_t size{};
    if (!take(size) || size > skirmish_save_hints_label_limit || bytes.size() - at != size) return std::nullopt;
    hints.label.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
    return hints;
}

} // namespace eawr::viewer
