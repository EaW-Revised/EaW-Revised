#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/assets/texture_chain.hpp"

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

// BC3 (DXT5) DDS files built in memory for the texture tests (#5989): a light-texture look (near black, small bright
// windows) so the chain generator has something to average.
namespace eawr::testing {

inline std::vector<std::uint8_t> light_texels(const std::uint32_t side) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(side) * side * 4U, 0);
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            std::uint8_t* const texel = &rgba[(static_cast<std::size_t>(y) * side + x) * 4U];
            const bool window = (x % 13U) < 3U && (y % 11U) < 2U;
            texel[0] = static_cast<std::uint8_t>(window ? 250U : (x + y) % 7U);
            texel[1] = static_cast<std::uint8_t>(window ? 210U : (x * 3U + y) % 5U);
            texel[2] = static_cast<std::uint8_t>(window ? 120U : 0U);
            texel[3] = 255;
        }
    }
    return rgba;
}

// A rough tangent-space normal map: normals tilted by a hash of the texel around +z, constant gloss in alpha.
inline std::vector<std::uint8_t> normal_texels(const std::uint32_t side) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(side) * side * 4U, 0);
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            std::uint32_t h = x * 374761393U + y * 668265263U;
            h = (h ^ (h >> 13U)) * 1274126177U;
            h ^= h >> 16U;
            const float nx = (static_cast<float>(h & 255U) / 255.0F - 0.5F) * 2.4F;
            const float ny = (static_cast<float>((h >> 8U) & 255U) / 255.0F - 0.5F) * 2.4F;
            const float length = std::sqrt(nx * nx + ny * ny + 1.0F);
            std::uint8_t* const texel = &rgba[(static_cast<std::size_t>(y) * side + x) * 4U];
            texel[0] = static_cast<std::uint8_t>(127.5F + nx / length * 127.5F + 0.5F);
            texel[1] = static_cast<std::uint8_t>(127.5F + ny / length * 127.5F + 0.5F);
            texel[2] = static_cast<std::uint8_t>(127.5F + 127.5F / length + 0.5F);
            texel[3] = 200;
        }
    }
    return rgba;
}

// The levels of `texture` as a DDS file (DXT5, `texture.mips.size()` levels).
inline std::vector<char> dds_bc3_bytes(const assets::Texture& texture) {
    std::vector<char> bytes(128, 0);
    const auto put = [&](const std::size_t offset, const std::uint32_t value) { std::memcpy(bytes.data() + offset, &value, 4); };
    std::memcpy(bytes.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1007U | (texture.mips.size() > 1 ? 0x20000U : 0U));
    put(12, texture.height);
    put(16, texture.width);
    put(20, static_cast<std::uint32_t>(texture.mips[0].bytes.size()));
    put(28, static_cast<std::uint32_t>(texture.mips.size()));
    put(76, 32);
    put(80, 4);
    std::memcpy(bytes.data() + 84, "DXT5", 4);
    put(108, 0x1000U | (texture.mips.size() > 1 ? 0x400000U : 0U));
    for (const assets::MipLevel& level : texture.mips)
        bytes.insert(bytes.end(), reinterpret_cast<const char*>(level.bytes.data()),
                     reinterpret_cast<const char*>(level.bytes.data()) + level.bytes.size());
    return bytes;
}

// A single-level BC3 texture of `side` x `side` from RGBA texels.
inline assets::Texture single_level_bc3_of(const std::uint32_t side, const std::vector<std::uint8_t>& rgba) {
    assets::Texture texture;
    texture.width = side;
    texture.height = side;
    texture.format = assets::PixelFormat::bc3;
    texture.has_alpha = true;
    assets::MipLevel level;
    level.width = side;
    level.height = side;
    level.row_pitch = ((side + 3U) / 4U) * 16U;
    level.bytes = assets::encode_blocks(assets::PixelFormat::bc3, rgba, side, side);
    texture.mips.push_back(std::move(level));
    return texture;
}

// A single-level light texture of `side` x `side`.
inline assets::Texture single_level_bc3(const std::uint32_t side) {
    assets::Texture texture;
    texture.width = side;
    texture.height = side;
    texture.format = assets::PixelFormat::bc3;
    texture.has_alpha = true;
    assets::MipLevel level;
    level.width = side;
    level.height = side;
    level.row_pitch = ((side + 3U) / 4U) * 16U;
    level.bytes = assets::encode_blocks(assets::PixelFormat::bc3, light_texels(side), side, side);
    texture.mips.push_back(std::move(level));
    return texture;
}

} // namespace eawr::testing
