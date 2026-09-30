#pragma once

#include "eawr/assets/assets.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

struct ParticleTexturePreparation final {
    std::vector<std::byte> bytes;
    std::string failure;

    explicit operator bool() const noexcept { return failure.empty(); }
};

// Godot Image::create_from_data consumes tightly packed, top-left-first mips.
// The asset decoder deliberately retains the source order, so normalize only
// this upload copy. Block compression is not row-addressable by pixel.
[[nodiscard]] inline ParticleTexturePreparation prepare_particle_texture(const assets::Texture& source) {
    auto reject = [](const char* reason) { return ParticleTexturePreparation{{}, reason}; };
    if (source.width == 0 || source.height == 0
        || source.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
        || source.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
        || source.mips.empty()) return reject("invalid particle texture dimensions or missing mips");

    std::size_t source_channels{};
    std::size_t upload_channels{};
    std::size_t block_bytes{};
    switch (source.format) {
    case assets::PixelFormat::rgba8:
    case assets::PixelFormat::bgra8: source_channels = upload_channels = 4; break;
    case assets::PixelFormat::bgr8: source_channels = upload_channels = 3; break;
    case assets::PixelFormat::l8: source_channels = upload_channels = 1; break;
    case assets::PixelFormat::a8:
        return reject("alpha-only particle texture upload is unsupported");
    case assets::PixelFormat::bc1:
    case assets::PixelFormat::bc4: block_bytes = 8; break;
    case assets::PixelFormat::bc2:
    case assets::PixelFormat::bc3:
    case assets::PixelFormat::bc5:
    case assets::PixelFormat::bc7: block_bytes = 16; break;
    default: return reject("unsupported particle texture pixel format");
    }
    if (block_bytes && source.source_origin == assets::ImageOrigin::bottom_left)
        return reject("bottom-left block-compressed particle texture is unsupported");
    if (source.source_origin != assets::ImageOrigin::top_left
        && source.source_origin != assets::ImageOrigin::bottom_left)
        return reject("invalid particle texture source origin");

    std::size_t upload_size{};
    std::uint32_t width = source.width;
    std::uint32_t height = source.height;
    constexpr std::size_t limit = static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
    for (std::size_t index = 0; index < source.mips.size(); ++index) {
        const assets::MipLevel& mip = source.mips[index];
        if (mip.width != width || mip.height != height)
            return reject("particle texture mip dimensions do not match the mip chain");
        const std::size_t columns = block_bytes ? (static_cast<std::size_t>(width) + 3) / 4 : width;
        const std::size_t rows = block_bytes ? (static_cast<std::size_t>(height) + 3) / 4 : height;
        const std::size_t source_stride = block_bytes ? block_bytes : source_channels;
        const std::size_t upload_stride = block_bytes ? block_bytes : upload_channels;
        if (columns > limit / source_stride || columns > limit / upload_stride)
            return reject("particle texture row size overflows upload limit");
        const std::size_t row_pitch = columns * source_stride;
        const std::size_t upload_pitch = columns * upload_stride;
        if (mip.row_pitch != row_pitch || rows > limit / row_pitch
            || mip.bytes.size() != row_pitch * rows)
            return reject("particle texture row pitch or byte length is invalid");
        if (rows > limit / upload_pitch || upload_size > limit - upload_pitch * rows)
            return reject("particle texture upload length overflows");
        upload_size += upload_pitch * rows;
        if (index + 1 < source.mips.size() && width == 1 && height == 1)
            return reject("particle texture has mips after the 1x1 level");
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }

    ParticleTexturePreparation result;
    result.bytes.reserve(upload_size);
    for (const assets::MipLevel& mip : source.mips) {
        if (block_bytes) {
            result.bytes.insert(result.bytes.end(), mip.bytes.begin(), mip.bytes.end());
            continue;
        }
        const std::size_t pitch = mip.row_pitch;
        for (std::size_t row = 0; row < mip.height; ++row) {
            const std::size_t source_row = source.source_origin == assets::ImageOrigin::bottom_left
                ? static_cast<std::size_t>(mip.height) - 1 - row : row;
            const std::size_t offset = source_row * pitch;
            for (std::size_t column = 0; column < mip.width; ++column) {
                const std::size_t pixel = offset + column * source_channels;
                if (source.format == assets::PixelFormat::bgr8
                    || source.format == assets::PixelFormat::bgra8) {
                    result.bytes.push_back(mip.bytes[pixel + 2]);
                    result.bytes.push_back(mip.bytes[pixel + 1]);
                    result.bytes.push_back(mip.bytes[pixel]);
                    if (source_channels == 4) result.bytes.push_back(mip.bytes[pixel + 3]);
                } else {
                    for (std::size_t channel = 0; channel < source_channels; ++channel)
                        result.bytes.push_back(mip.bytes[pixel + channel]);
                }
            }
        }
    }
    return result;
}

} // namespace eawr::presentation::godot_backend::detail
