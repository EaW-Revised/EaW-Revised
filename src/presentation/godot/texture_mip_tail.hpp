#pragma once

#include "eawr/assets/assets.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <utility>

namespace eawr::presentation::godot_backend::detail {

[[nodiscard]] inline std::uint32_t texture_block_bytes(const assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::bc1: case assets::PixelFormat::bc4: return 8;
    case assets::PixelFormat::bc2: case assets::PixelFormat::bc3:
    case assets::PixelFormat::bc5: case assets::PixelFormat::bc7: return 16;
    default: return 0;
    }
}

[[nodiscard]] inline std::uint32_t texture_channels(const assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::rgba8: case assets::PixelFormat::bgra8: return 4;
    case assets::PixelFormat::bgr8: return 3;
    default: return 1;
    }
}

// A single-level source remains unmipped. Only an authored multi-level chain
// needs a tail to satisfy Godot's all-or-nothing mip flag (MT-01).
[[nodiscard]] inline std::uint64_t texture_mip_tail_bytes(const assets::Texture& texture) {
    if (texture.mips.size() < 2) return 0;
    auto width = texture.mips.back().width;
    auto height = texture.mips.back().height;
    const auto block = texture_block_bytes(texture.format);
    std::uint64_t bytes{};
    while (width > 1 || height > 1) {
        width = std::max(1U, width / 2);
        height = std::max(1U, height / 2);
        bytes += block ? ((static_cast<std::uint64_t>(width) + 3) / 4)
            * ((static_cast<std::uint64_t>(height) + 3) / 4) * block
            : static_cast<std::uint64_t>(width) * height * texture_channels(texture.format);
    }
    return bytes;
}

// Validated, tightly packed source mips only. Authored bytes stay untouched.
// Block replication preserves a valid compressed encoding without a lossy
// decode/re-encode of the authored levels. The missing tail is approximate.
[[nodiscard]] inline std::vector<std::byte> texture_mip_tail(const assets::Texture& texture,
    const assets::MipLevel* normalized_last = nullptr) {
    if (texture_mip_tail_bytes(texture) == 0) return {};
    std::vector<std::byte> result;
    result.reserve(static_cast<std::size_t>(texture_mip_tail_bytes(texture)));
    assets::MipLevel previous = normalized_last ? *normalized_last : texture.mips.back();
    const auto block = texture_block_bytes(texture.format);
    const auto channels = texture_channels(texture.format);
    const auto source_columns = block ? (static_cast<std::uint64_t>(previous.width) + 3) / 4 : previous.width;
    const auto source_rows = block ? (static_cast<std::uint64_t>(previous.height) + 3) / 4 : previous.height;
    if (previous.width == 0 || previous.height == 0
        || previous.row_pitch != source_columns * (block ? block : channels)
        || previous.bytes.size() != static_cast<std::uint64_t>(previous.row_pitch) * source_rows) return {};
    while (previous.width > 1 || previous.height > 1) {
        assets::MipLevel next;
        next.width = std::max(1U, previous.width / 2);
        next.height = std::max(1U, previous.height / 2);
        const auto columns = block ? (next.width + 3) / 4 : next.width;
        const auto rows = block ? (next.height + 3) / 4 : next.height;
        next.row_pitch = columns * (block ? block : channels);
        next.bytes.resize(static_cast<std::size_t>(next.row_pitch) * rows);
        for (std::uint32_t y = 0; y < rows; ++y) {
            for (std::uint32_t x = 0; x < columns; ++x) {
                const auto destination = static_cast<std::size_t>(y) * next.row_pitch
                    + static_cast<std::size_t>(x) * (block ? block : channels);
                if (block) {
                    const auto sx = std::min(x * 2, (previous.width + 3) / 4 - 1);
                    const auto sy = std::min(y * 2, (previous.height + 3) / 4 - 1);
                    const auto offset = static_cast<std::size_t>(sy) * previous.row_pitch
                        + static_cast<std::size_t>(sx) * block;
                    std::copy_n(previous.bytes.begin() + static_cast<std::ptrdiff_t>(offset), block,
                        next.bytes.begin() + static_cast<std::ptrdiff_t>(destination));
                } else {
                    for (std::uint32_t channel = 0; channel < channels; ++channel) {
                        unsigned sum{}, count{};
                        // Proportional bounds include odd edges and 1-pixel axes.
                        for (auto sy = y * previous.height / next.height;
                             sy < (y + 1) * previous.height / next.height; ++sy) {
                            for (auto sx = x * previous.width / next.width;
                                 sx < (x + 1) * previous.width / next.width; ++sx) {
                                sum += std::to_integer<unsigned>(previous.bytes[
                                    static_cast<std::size_t>(sy) * previous.row_pitch + sx * channels + channel]);
                                ++count;
                            }
                        }
                        next.bytes[destination + channel] = static_cast<std::byte>((sum + count / 2) / count);
                    }
                }
            }
        }
        result.insert(result.end(), next.bytes.begin(), next.bytes.end());
        previous = std::move(next);
    }
    return result;
}

} // namespace eawr::presentation::godot_backend::detail
