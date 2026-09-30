#include "eawr/presentation/ui/hud.hpp"

#include <array>

namespace eawr::presentation::ui {
core::Result<ShellAlphaMask> shell_alpha_mask(const assets::Texture& texture) {
    const auto fail = [&]() {
        core::Diagnostic d;
        d.code = "EAWR-UI-0314";
        d.severity = core::Severity::warning;
        d.message = "faceplate texture alpha format or mip layout is unsupported";
        d.logical_path = texture.source.logical_path;
        return core::Result<ShellAlphaMask>::failure(std::move(d));
    };
    if (texture.mips.empty()) return fail();
    const auto& mip = texture.mips.front();
    if (mip.width == 0 || mip.height == 0 || mip.width > 16384U || mip.height > 16384U) return fail();
    using F = assets::PixelFormat;
    const auto format = texture.format;
    const bool block = format == F::bc1 || format == F::bc2 || format == F::bc3;
    std::size_t stride = 0;
    if (format == F::rgba8 || format == F::bgra8) stride = 4;
    if (format == F::bgr8) stride = 3;
    if (format == F::l8 || format == F::a8) stride = 1;
    if (!block && stride == 0) return fail();
    const std::size_t block_size = format == F::bc1 ? 8U : 16U;
    const std::size_t rows = block ? (mip.height+3U)/4U : mip.height;
    const std::size_t row_bytes = block ? ((mip.width+3U)/4U)*block_size : mip.width*stride;
    if (mip.row_pitch < row_bytes || mip.bytes.size() < rows*mip.row_pitch) return fail();
    const auto byte = [&](std::size_t offset) { return std::to_integer<std::uint8_t>(mip.bytes[offset]); };
    ShellAlphaMask mask{mip.width,mip.height,{}};
    mask.alpha.resize(static_cast<std::size_t>(mip.width)*mip.height,255);
    for (std::uint32_t y = 0; y < mip.height; ++y) {
        const auto source_y = texture.source_origin == assets::ImageOrigin::bottom_left ? mip.height-1U-y : y;
        for (std::uint32_t x = 0; x < mip.width; ++x) {
            std::uint8_t alpha = 255;
            if (!block) {
                const auto offset = static_cast<std::size_t>(source_y)*mip.row_pitch+x*stride;
                if (texture.has_alpha && (format == F::rgba8 || format == F::bgra8)) alpha = byte(offset+3U);
                if (format == F::a8) alpha = byte(offset);
            } else {
                const auto offset = static_cast<std::size_t>(source_y/4U)*mip.row_pitch+(x/4U)*block_size;
                const auto pixel = (source_y%4U)*4U+x%4U;
                if (format == F::bc1) {
                    const unsigned c0 = byte(offset) | (static_cast<unsigned>(byte(offset+1U))<<8U);
                    const unsigned c1 = byte(offset+2U) | (static_cast<unsigned>(byte(offset+3U))<<8U);
                    const auto code = (byte(offset+4U+pixel/4U) >> ((pixel%4U)*2U)) & 3U;
                    if (c0 <= c1 && code == 3U) alpha = 0;
                } else if (format == F::bc2) {
                    alpha = static_cast<std::uint8_t>(((byte(offset+pixel/2U) >> ((pixel%2U)*4U)) & 15U)*17U);
                } else {
                    std::array<unsigned,8> table{byte(offset),byte(offset+1U)};
                    if (table[0] > table[1]) {
                        for (unsigned i = 1; i <= 6; ++i) table[i+1U] = ((7U-i)*table[0]+i*table[1])/7U;
                    } else {
                        for (unsigned i = 1; i <= 4; ++i) table[i+1U] = ((5U-i)*table[0]+i*table[1])/5U;
                        table[6] = 0; table[7] = 255;
                    }
                    std::uint64_t bits = 0;
                    for (unsigned i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(byte(offset+2U+i)) << (i*8U);
                    alpha = static_cast<std::uint8_t>(table[(bits >> (pixel*3U)) & 7U]);
                }
            }
            mask.alpha[static_cast<std::size_t>(y)*mip.width+x] = alpha;
        }
    }
    return core::Result<ShellAlphaMask>::success(std::move(mask));
}
} // namespace eawr::presentation::ui
