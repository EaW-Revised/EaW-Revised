#include "eawr/assets/texture_chain.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

// Block layouts (public S3TC documentation), texels in raster order inside a 4x4 block:
//   BC1  8 bytes:  colour endpoints c0, c1 (RGB565, little-endian), sixteen 2-bit colour indices (texel 0 in the low bits)
//   BC2 16 bytes:  sixteen explicit 4-bit alphas (texel 0 in the low nibble of byte 0), then the BC1 colour block
//   BC3 16 bytes:  alpha endpoints a0, a1, sixteen 3-bit alpha indices (little-endian), then the BC1 colour block
// Colour: c0 > c1 selects four values (c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1). In BC1 only, c0 <= c1 selects three
// (c0, c1, their mean) and a black fourth; BC2/BC3 always use the four-value mode. BC3 alpha: a0 > a1 selects eight values
// (a0, a1 and six interpolated); a0 <= a1 selects six (a0, a1, four interpolated, 0 and 255).
// Retail's code and D3DX are not used or consulted; the only retail fact used is that levels are plain box averages.

namespace eawr::assets {
namespace {

[[nodiscard]] constexpr std::uint32_t blocks_across(const std::uint32_t texels) noexcept { return (texels + 3U) / 4U; }
[[nodiscard]] constexpr std::uint32_t half_of(const std::uint32_t texels) noexcept { return std::max(1U, texels / 2U); }

// Bytes of one 4x4 block, 0 for a format that is not block compressed or not handled here.
[[nodiscard]] constexpr std::uint32_t block_size_of(const PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::bc1: return 8U;
    case PixelFormat::bc2: case PixelFormat::bc3: return 16U;
    default: return 0U;
    }
}

// Bytes of one texel of the uncompressed formats handled here, 0 otherwise.
[[nodiscard]] constexpr std::uint32_t texel_size_of(const PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::rgba8: case PixelFormat::bgra8: return 4U;
    case PixelFormat::bgr8: return 3U;
    default: return 0U;
    }
}

struct Texel final {
    std::array<std::uint8_t, 4> channel{};
};
using BlockTexels = std::array<Texel, 16>;

[[nodiscard]] std::uint8_t byte_of(const std::byte value) noexcept { return static_cast<std::uint8_t>(value); }

[[nodiscard]] std::array<std::uint8_t, 3> expand_565(const std::uint32_t colour) noexcept {
    const std::uint32_t r5 = (colour >> 11U) & 31U;
    const std::uint32_t g6 = (colour >> 5U) & 63U;
    const std::uint32_t b5 = colour & 31U;
    return {static_cast<std::uint8_t>((r5 << 3U) | (r5 >> 2U)), static_cast<std::uint8_t>((g6 << 2U) | (g6 >> 4U)),
            static_cast<std::uint8_t>((b5 << 3U) | (b5 >> 2U))};
}

[[nodiscard]] std::array<std::array<std::uint8_t, 3>, 4> colour_palette(const std::uint32_t c0, const std::uint32_t c1) noexcept {
    std::array<std::array<std::uint8_t, 3>, 4> palette{};
    palette[0] = expand_565(c0);
    palette[1] = expand_565(c1);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const std::uint32_t a = palette[0][channel];
        const std::uint32_t b = palette[1][channel];
        palette[2][channel] = static_cast<std::uint8_t>((2U * a + b) / 3U);
        palette[3][channel] = static_cast<std::uint8_t>((a + 2U * b) / 3U);
    }
    return palette;
}

[[nodiscard]] std::array<std::uint8_t, 8> alpha_palette(const std::uint32_t a0, const std::uint32_t a1) noexcept {
    std::array<std::uint8_t, 8> palette{};
    palette[0] = static_cast<std::uint8_t>(a0);
    palette[1] = static_cast<std::uint8_t>(a1);
    if (a0 > a1) {
        for (std::uint32_t i = 2; i < 8; ++i) palette[i] = static_cast<std::uint8_t>(((8U - i) * a0 + (i - 1U) * a1) / 7U);
    } else {
        for (std::uint32_t i = 2; i < 6; ++i) palette[i] = static_cast<std::uint8_t>(((6U - i) * a0 + (i - 1U) * a1) / 5U);
        palette[6] = 0;
        palette[7] = 255;
    }
    return palette;
}

// The 8-byte colour block at `block`, opaque. `bc1`: the three-colour mode (c0 <= c1) applies.
void decode_colour(const std::byte* const block, const bool bc1, BlockTexels& texels) noexcept {
    const std::uint32_t c0 = byte_of(block[0]) | (static_cast<std::uint32_t>(byte_of(block[1])) << 8U);
    const std::uint32_t c1 = byte_of(block[2]) | (static_cast<std::uint32_t>(byte_of(block[3])) << 8U);
    auto palette = colour_palette(c0, c1);
    if (bc1 && c0 <= c1) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            palette[2][channel] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(palette[0][channel]) + palette[1][channel]) / 2U);
            palette[3][channel] = 0;
        }
    }
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 4; ++i) bits |= static_cast<std::uint32_t>(byte_of(block[4 + i])) << (8U * i);
    for (std::size_t i = 0; i < 16; ++i) {
        const auto index = (bits >> (2U * i)) & 3U;
        for (std::size_t channel = 0; channel < 3; ++channel) texels[i].channel[channel] = palette[index][channel];
        texels[i].channel[3] = 255;
    }
}

void decode_block(const PixelFormat format, const std::byte* const block, BlockTexels& texels) noexcept {
    if (format == PixelFormat::bc1) {
        decode_colour(block, true, texels);
        return;
    }
    decode_colour(block + 8, false, texels);
    if (format == PixelFormat::bc2) {
        for (std::size_t i = 0; i < 16; ++i) {
            const std::uint32_t nibble = (byte_of(block[i / 2U]) >> (4U * (i % 2U))) & 15U;
            texels[i].channel[3] = static_cast<std::uint8_t>(nibble * 17U);
        }
        return;
    }
    const auto palette = alpha_palette(byte_of(block[0]), byte_of(block[1]));
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(byte_of(block[2 + i])) << (8U * i);
    for (std::size_t i = 0; i < 16; ++i) texels[i].channel[3] = palette[static_cast<std::size_t>((bits >> (3U * i)) & 7U)];
}

[[nodiscard]] std::uint32_t squared_distance(const std::array<std::uint8_t, 3>& a, const Texel& b) noexcept {
    std::uint32_t sum = 0;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const auto d = static_cast<std::int32_t>(a[channel]) - static_cast<std::int32_t>(b.channel[channel]);
        sum += static_cast<std::uint32_t>(d * d);
    }
    return sum;
}

struct ColourFit final {
    std::uint32_t c0{};
    std::uint32_t c1{};
    std::uint32_t indices{};
    std::uint64_t error{std::numeric_limits<std::uint64_t>::max()};
};

[[nodiscard]] std::uint32_t quantise_565(const std::array<float, 3>& rgb) noexcept {
    const auto clamp_to = [](const float value, const float high) { return std::clamp(value, 0.0F, high); };
    const auto r = static_cast<std::uint32_t>(clamp_to(rgb[0] * (31.0F / 255.0F) + 0.5F, 31.0F));
    const auto g = static_cast<std::uint32_t>(clamp_to(rgb[1] * (63.0F / 255.0F) + 0.5F, 63.0F));
    const auto b = static_cast<std::uint32_t>(clamp_to(rgb[2] * (31.0F / 255.0F) + 0.5F, 31.0F));
    return (r << 11U) | (g << 5U) | b;
}

// The best indices for endpoints (a, b); c0 is the larger value as BC1-style decoders expect.
[[nodiscard]] ColourFit fit_endpoints(const BlockTexels& texels, std::uint32_t a, std::uint32_t b) noexcept {
    ColourFit fit;
    if (a < b) std::swap(a, b);
    fit.c0 = a;
    fit.c1 = b;
    const auto palette = colour_palette(a, b);
    fit.error = 0;
    fit.indices = 0;
    const std::uint32_t usable = a == b ? 1U : 4U; // equal endpoints: one value, every index 0
    for (std::size_t i = 0; i < 16; ++i) {
        std::uint32_t best = 0;
        std::uint32_t best_distance = squared_distance(palette[0], texels[i]);
        for (std::uint32_t k = 1; k < usable; ++k) {
            const std::uint32_t distance = squared_distance(palette[k], texels[i]);
            if (distance < best_distance) {
                best_distance = distance;
                best = k;
            }
        }
        fit.error += best_distance;
        fit.indices |= best << (2U * i);
    }
    return fit;
}

// Least squares: the endpoints that best reproduce the texels under the indices already chosen, then the best indices
// for those (quantised) endpoints. Kept only while the error falls; two rounds at most.
[[nodiscard]] ColourFit refine_endpoints(const BlockTexels& texels, ColourFit best) noexcept {
    constexpr std::array<float, 4> weight{1.0F, 0.0F, 2.0F / 3.0F, 1.0F / 3.0F}; // share of c0 in each palette entry
    for (int round = 0; round < 2 && best.c0 != best.c1; ++round) {
        float a11 = 0.0F, a12 = 0.0F, a22 = 0.0F;
        std::array<float, 3> b1{}, b2{};
        for (std::size_t i = 0; i < 16; ++i) {
            const float alpha = weight[(best.indices >> (2U * i)) & 3U];
            const float beta = 1.0F - alpha;
            a11 += alpha * alpha;
            a12 += alpha * beta;
            a22 += beta * beta;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const auto value = static_cast<float>(texels[i].channel[channel]);
                b1[channel] += alpha * value;
                b2[channel] += beta * value;
            }
        }
        const float determinant = a11 * a22 - a12 * a12;
        if (!(std::fabs(determinant) > 1e-4F)) break;
        std::array<float, 3> first{}, second{};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            first[channel] = (b1[channel] * a22 - b2[channel] * a12) / determinant;
            second[channel] = (b2[channel] * a11 - b1[channel] * a12) / determinant;
        }
        const ColourFit candidate = fit_endpoints(texels, quantise_565(first), quantise_565(second));
        if (candidate.error >= best.error) break;
        best = candidate;
    }
    return best;
}

// Two candidate endpoint pairs, the better one kept: the bounding box's diagonal and the principal axis of the colours.
// Light textures are mostly near-black with a few saturated points, so both matter.
[[nodiscard]] ColourFit fit_colour(const BlockTexels& texels) noexcept {
    std::array<float, 3> low{255.0F, 255.0F, 255.0F};
    std::array<float, 3> high{0.0F, 0.0F, 0.0F};
    std::array<float, 3> mean{0.0F, 0.0F, 0.0F};
    for (const Texel& texel : texels) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto value = static_cast<float>(texel.channel[channel]);
            low[channel] = std::min(low[channel], value);
            high[channel] = std::max(high[channel], value);
            mean[channel] += value * (1.0F / 16.0F);
        }
    }
    ColourFit best = fit_endpoints(texels, quantise_565(high), quantise_565(low));
    if (best.error == 0) return best;
    if (low == high) {
        // One colour: the 565 grid is coarse (steps of 8, 4, 8 stored values), so the interpolated entries can land
        // closer than either endpoint. Try the colour as the first endpoint, as 2/3 of it (against black) and as 1/3 of it.
        const std::array<float, 3> scale2{high[0] * 1.5F, high[1] * 1.5F, high[2] * 1.5F};
        const std::array<float, 3> scale3{high[0] * 3.0F, high[1] * 3.0F, high[2] * 3.0F};
        for (const auto& candidate : {scale2, scale3}) {
            const ColourFit fit = fit_endpoints(texels, quantise_565(candidate), 0U);
            if (fit.error < best.error) best = fit;
        }
        return best;
    }

    // Principal axis by power iteration on the 3x3 covariance.
    std::array<float, 6> covariance{}; // xx xy xz yy yz zz
    for (const Texel& texel : texels) {
        const float x = static_cast<float>(texel.channel[0]) - mean[0];
        const float y = static_cast<float>(texel.channel[1]) - mean[1];
        const float z = static_cast<float>(texel.channel[2]) - mean[2];
        covariance[0] += x * x;
        covariance[1] += x * y;
        covariance[2] += x * z;
        covariance[3] += y * y;
        covariance[4] += y * z;
        covariance[5] += z * z;
    }
    std::array<float, 3> axis{high[0] - low[0], high[1] - low[1], high[2] - low[2]};
    for (int iteration = 0; iteration < 6; ++iteration) {
        const std::array<float, 3> next{covariance[0] * axis[0] + covariance[1] * axis[1] + covariance[2] * axis[2],
                                        covariance[1] * axis[0] + covariance[3] * axis[1] + covariance[4] * axis[2],
                                        covariance[2] * axis[0] + covariance[4] * axis[1] + covariance[5] * axis[2]};
        const float length = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (!(length > 1e-6F)) break;
        axis = {next[0] / length, next[1] / length, next[2] / length};
    }
    const float axis_length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (!(axis_length > 1e-6F)) return best;
    for (float& component : axis) component /= axis_length;
    float t_low = std::numeric_limits<float>::max();
    float t_high = std::numeric_limits<float>::lowest();
    for (const Texel& texel : texels) {
        float t = 0.0F;
        for (std::size_t channel = 0; channel < 3; ++channel) t += (static_cast<float>(texel.channel[channel]) - mean[channel]) * axis[channel];
        t_low = std::min(t_low, t);
        t_high = std::max(t_high, t);
    }
    const std::array<float, 3> end_high{mean[0] + axis[0] * t_high, mean[1] + axis[1] * t_high, mean[2] + axis[2] * t_high};
    const std::array<float, 3> end_low{mean[0] + axis[0] * t_low, mean[1] + axis[1] * t_low, mean[2] + axis[2] * t_low};
    const ColourFit principal = fit_endpoints(texels, quantise_565(end_high), quantise_565(end_low));
    if (principal.error < best.error) best = principal;
    return refine_endpoints(texels, best);
}


void encode_alpha_bc3(const BlockTexels& texels, std::byte* const out) noexcept {
    std::uint32_t low = 255;
    std::uint32_t high = 0;
    for (const Texel& texel : texels) {
        low = std::min<std::uint32_t>(low, texel.channel[3]);
        high = std::max<std::uint32_t>(high, texel.channel[3]);
    }
    std::uint64_t bits = 0;
    if (high != low) { // else every index stays 0 and both endpoints are the value
        const auto palette = alpha_palette(high, low);
        for (std::size_t i = 0; i < 16; ++i) {
            std::uint32_t best = 0;
            std::uint32_t best_distance = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t k = 0; k < 8; ++k) {
                const auto d = static_cast<std::int32_t>(palette[k]) - static_cast<std::int32_t>(texels[i].channel[3]);
                const auto distance = static_cast<std::uint32_t>(d * d);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = k;
                }
            }
            bits |= static_cast<std::uint64_t>(best) << (3U * i);
        }
    }
    out[0] = static_cast<std::byte>(high);
    out[1] = static_cast<std::byte>(low);
    for (std::size_t i = 0; i < 6; ++i) out[2 + i] = static_cast<std::byte>((bits >> (8U * i)) & 0xFFU);
}

void encode_alpha_bc2(const BlockTexels& texels, std::byte* const out) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        const std::uint32_t low = (texels[2 * i].channel[3] * 15U + 127U) / 255U;
        const std::uint32_t high = (texels[2 * i + 1].channel[3] * 15U + 127U) / 255U;
        out[i] = static_cast<std::byte>(low | (high << 4U));
    }
}

void encode_colour(const BlockTexels& texels, std::byte* const out) noexcept {
    const ColourFit fit = fit_colour(texels);
    out[0] = static_cast<std::byte>(fit.c0 & 0xFFU);
    out[1] = static_cast<std::byte>(fit.c0 >> 8U);
    out[2] = static_cast<std::byte>(fit.c1 & 0xFFU);
    out[3] = static_cast<std::byte>(fit.c1 >> 8U);
    for (std::size_t i = 0; i < 4; ++i) out[4 + i] = static_cast<std::byte>((fit.indices >> (8U * i)) & 0xFFU);
}

void encode_block(const PixelFormat format, const BlockTexels& texels, std::byte* const out) noexcept {
    if (format == PixelFormat::bc1) {
        encode_colour(texels, out);
        return;
    }
    if (format == PixelFormat::bc2) encode_alpha_bc2(texels, out);
    else encode_alpha_bc3(texels, out);
    encode_colour(texels, out + 8);
}

[[nodiscard]] std::uint8_t average4(const std::uint32_t a, const std::uint32_t b, const std::uint32_t c, const std::uint32_t d) noexcept {
    return static_cast<std::uint8_t>((a + b + c + d + 2U) >> 2U);
}

// One destination row of the box filter from two source rows of `width` RGBA texels (the same row twice at an odd edge).
void halve_row(const std::uint8_t* const source0, const std::uint8_t* const source1, const std::uint32_t width,
               std::uint8_t* const destination) noexcept {
    const std::uint32_t half = half_of(width);
    for (std::uint32_t x = 0; x < half; ++x) {
        const std::uint32_t x0 = std::min(2U * x, width - 1U);
        const std::uint32_t x1 = std::min(2U * x + 1U, width - 1U);
        for (std::uint32_t channel = 0; channel < 4; ++channel) {
            destination[4U * x + channel] = average4(source0[4U * x0 + channel], source0[4U * x1 + channel],
                                                     source1[4U * x0 + channel], source1[4U * x1 + channel]);
        }
    }
}

// One row of an uncompressed level as RGBA.
void unpack_row(const PixelFormat format, const MipLevel& level, const std::uint32_t y, std::uint8_t* const destination) noexcept {
    const std::uint32_t size = texel_size_of(format);
    const std::byte* const row = level.bytes.data() + static_cast<std::size_t>(y) * level.row_pitch;
    for (std::uint32_t x = 0; x < level.width; ++x) {
        const std::byte* const texel = row + static_cast<std::size_t>(x) * size;
        std::uint8_t* const out = destination + 4U * x;
        if (format == PixelFormat::rgba8) {
            out[0] = byte_of(texel[0]); out[1] = byte_of(texel[1]); out[2] = byte_of(texel[2]); out[3] = byte_of(texel[3]);
        } else if (format == PixelFormat::bgra8) {
            out[0] = byte_of(texel[2]); out[1] = byte_of(texel[1]); out[2] = byte_of(texel[0]); out[3] = byte_of(texel[3]);
        } else { // bgr8
            out[0] = byte_of(texel[2]); out[1] = byte_of(texel[1]); out[2] = byte_of(texel[0]); out[3] = 255;
        }
    }
}

// The first halving straight from the stored level: a block row (BC) or two rows (uncompressed) are decoded at a time, so
// the full-size RGBA image is never held. The result equals box_halve_rgba of the whole decoded level.
[[nodiscard]] std::vector<std::uint8_t> halve_stored_level(const PixelFormat format, const MipLevel& level) {
    const std::uint32_t width = level.width;
    const std::uint32_t height = level.height;
    const std::uint32_t half_width = half_of(width);
    const std::uint32_t half_height = half_of(height);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(half_width) * half_height * 4U);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
    if (block_size_of(format) == 0U) {
        std::vector<std::uint8_t> rows(row_bytes * 2U);
        for (std::uint32_t y = 0; y < half_height; ++y) {
            unpack_row(format, level, std::min(2U * y, height - 1U), rows.data());
            unpack_row(format, level, std::min(2U * y + 1U, height - 1U), rows.data() + row_bytes);
            halve_row(rows.data(), rows.data() + row_bytes, width, result.data() + static_cast<std::size_t>(y) * half_width * 4U);
        }
        return result;
    }
    const std::uint32_t block_bytes = block_size_of(format);
    const std::uint32_t columns = blocks_across(width);
    const std::uint32_t rows = blocks_across(height);
    const std::size_t strip_pitch = static_cast<std::size_t>(columns) * 16U; // bytes per strip row
    std::vector<std::uint8_t> strip(strip_pitch * 4U);
    BlockTexels texels;
    for (std::uint32_t block_row = 0; block_row < rows; ++block_row) {
        for (std::uint32_t block_column = 0; block_column < columns; ++block_column) {
            decode_block(format, level.bytes.data() + (static_cast<std::size_t>(block_row) * columns + block_column) * block_bytes, texels);
            for (std::size_t i = 0; i < 16; ++i) {
                std::uint8_t* const target = strip.data() + (i / 4U) * strip_pitch + (static_cast<std::size_t>(block_column) * 4U + i % 4U) * 4U;
                for (std::size_t channel = 0; channel < 4; ++channel) target[channel] = texels[i].channel[channel];
            }
        }
        for (std::uint32_t y = 2U * block_row; y < 2U * block_row + 2U && y < half_height; ++y) {
            const std::uint32_t y0 = std::min(2U * y, height - 1U) - 4U * block_row;
            const std::uint32_t y1 = std::min(2U * y + 1U, height - 1U) - 4U * block_row;
            halve_row(strip.data() + y0 * strip_pitch, strip.data() + y1 * strip_pitch, width,
                      result.data() + static_cast<std::size_t>(y) * half_width * 4U);
        }
    }
    return result;
}

// RGBA texels to the stored form of one level.
[[nodiscard]] MipLevel store_level(const PixelFormat format, const std::vector<std::uint8_t>& rgba, const std::uint32_t width,
                                   const std::uint32_t height) {
    MipLevel level;
    level.width = width;
    level.height = height;
    if (block_size_of(format) != 0U) {
        level.row_pitch = blocks_across(width) * block_size_of(format);
        level.bytes = encode_blocks(format, rgba, width, height);
        return level;
    }
    const std::uint32_t size = texel_size_of(format);
    level.row_pitch = width * size;
    level.bytes.resize(static_cast<std::size_t>(level.row_pitch) * height);
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
        const std::uint8_t* const in = rgba.data() + 4U * i;
        std::byte* const out = level.bytes.data() + size * i;
        if (format == PixelFormat::rgba8) {
            out[0] = static_cast<std::byte>(in[0]); out[1] = static_cast<std::byte>(in[1]);
            out[2] = static_cast<std::byte>(in[2]); out[3] = static_cast<std::byte>(in[3]);
        } else if (format == PixelFormat::bgra8) {
            out[0] = static_cast<std::byte>(in[2]); out[1] = static_cast<std::byte>(in[1]);
            out[2] = static_cast<std::byte>(in[0]); out[3] = static_cast<std::byte>(in[3]);
        } else { // bgr8
            out[0] = static_cast<std::byte>(in[2]); out[1] = static_cast<std::byte>(in[1]); out[2] = static_cast<std::byte>(in[0]);
        }
    }
    return level;
}

[[nodiscard]] std::uint64_t level_bytes(const PixelFormat format, const std::uint32_t width, const std::uint32_t height) noexcept {
    if (const std::uint32_t block = block_size_of(format); block != 0U)
        return static_cast<std::uint64_t>(blocks_across(width)) * blocks_across(height) * block;
    return static_cast<std::uint64_t>(width) * height * texel_size_of(format);
}

// Formats this file completes: BC2, BC3, BC1 without 1-bit alpha, and the 24- and 32-bit uncompressed ones.
[[nodiscard]] bool handled_format(const Texture& texture) noexcept {
    switch (texture.format) {
    case PixelFormat::bc1: return !texture.has_alpha;
    case PixelFormat::bc2: case PixelFormat::bc3: case PixelFormat::rgba8: case PixelFormat::bgra8: case PixelFormat::bgr8: return true;
    default: return false;
    }
}

// Whether `texture` is of a handled format with tightly packed levels that halve one another.
[[nodiscard]] bool well_formed(const Texture& texture) noexcept {
    // At most 32 levels: the file loader's own limit, and what Source::authored_levels can name.
    if (!handled_format(texture) || texture.mips.empty() || texture.mips.size() > 32U || texture.width == 0 || texture.height == 0) return false;
    std::uint32_t width = texture.width;
    std::uint32_t height = texture.height;
    for (const MipLevel& level : texture.mips) {
        if (level.width != width || level.height != height) return false;
        const std::uint32_t block = block_size_of(texture.format);
        const std::uint64_t pitch = block != 0U ? static_cast<std::uint64_t>(blocks_across(width)) * block
                                                : static_cast<std::uint64_t>(width) * texel_size_of(texture.format);
        const std::uint64_t rows = block != 0U ? blocks_across(height) : height;
        if (level.row_pitch != pitch || level.bytes.size() != pitch * rows) return false;
        width = half_of(width);
        height = half_of(height);
    }
    return true;
}

} // namespace

bool mip_chain_incomplete(const Texture& texture) noexcept {
    if (!well_formed(texture)) return false;
    const MipLevel& last = texture.mips.back();
    return last.width > 1U || last.height > 1U;
}

std::uint64_t mip_chain_added_bytes(const Texture& texture) noexcept {
    if (!mip_chain_incomplete(texture)) return 0;
    std::uint32_t width = texture.mips.back().width;
    std::uint32_t height = texture.mips.back().height;
    std::uint64_t bytes = 0;
    while (width > 1U || height > 1U) {
        width = half_of(width);
        height = half_of(height);
        bytes += level_bytes(texture.format, width, height);
    }
    return bytes;
}

std::vector<std::uint8_t> decode_blocks(const PixelFormat format, const std::span<const std::byte> blocks, const std::uint32_t width,
                                        const std::uint32_t height) {
    const std::uint32_t block_bytes = block_size_of(format);
    if (block_bytes == 0U) return {};
    const std::uint32_t columns = blocks_across(width);
    const std::uint32_t rows = blocks_across(height);
    if (blocks.size() < static_cast<std::size_t>(columns) * rows * block_bytes) return {};
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U);
    BlockTexels texels;
    for (std::uint32_t block_row = 0; block_row < rows; ++block_row) {
        for (std::uint32_t block_column = 0; block_column < columns; ++block_column) {
            decode_block(format, blocks.data() + (static_cast<std::size_t>(block_row) * columns + block_column) * block_bytes, texels);
            for (std::uint32_t i = 0; i < 16; ++i) {
                const std::uint32_t x = block_column * 4U + i % 4U;
                const std::uint32_t y = block_row * 4U + i / 4U;
                if (x >= width || y >= height) continue;
                for (std::size_t channel = 0; channel < 4; ++channel) rgba[(static_cast<std::size_t>(y) * width + x) * 4U + channel] = texels[i].channel[channel];
            }
        }
    }
    return rgba;
}

std::vector<std::byte> encode_blocks(const PixelFormat format, const std::span<const std::uint8_t> rgba, const std::uint32_t width,
                                     const std::uint32_t height) {
    const std::uint32_t block_bytes = block_size_of(format);
    if (block_bytes == 0U || width == 0 || height == 0 || rgba.size() != static_cast<std::size_t>(width) * height * 4U) return {};
    const std::uint32_t columns = blocks_across(width);
    const std::uint32_t rows = blocks_across(height);
    std::vector<std::byte> blocks(static_cast<std::size_t>(columns) * rows * block_bytes);
    BlockTexels texels;
    for (std::uint32_t block_row = 0; block_row < rows; ++block_row) {
        for (std::uint32_t block_column = 0; block_column < columns; ++block_column) {
            for (std::uint32_t i = 0; i < 16; ++i) {
                const std::uint32_t x = std::min(block_column * 4U + i % 4U, width - 1U);
                const std::uint32_t y = std::min(block_row * 4U + i / 4U, height - 1U);
                for (std::size_t channel = 0; channel < 4; ++channel) texels[i].channel[channel] = rgba[(static_cast<std::size_t>(y) * width + x) * 4U + channel];
            }
            encode_block(format, texels, blocks.data() + (static_cast<std::size_t>(block_row) * columns + block_column) * block_bytes);
        }
    }
    return blocks;
}

std::vector<std::uint8_t> halve_level_rgba(const PixelFormat format, const MipLevel& level) {
    if (level.width == 0 || level.height == 0) return {};
    const std::uint32_t block = block_size_of(format);
    const std::uint32_t size = texel_size_of(format);
    if (block == 0U && size == 0U) return {};
    const std::uint64_t pitch = block != 0U ? static_cast<std::uint64_t>(blocks_across(level.width)) * block : static_cast<std::uint64_t>(level.width) * size;
    const std::uint64_t rows = block != 0U ? blocks_across(level.height) : level.height;
    if (level.row_pitch != pitch || level.bytes.size() != pitch * rows) return {};
    return halve_stored_level(format, level);
}

std::vector<std::uint8_t> box_halve_rgba(const std::span<const std::uint8_t> rgba, const std::uint32_t width, const std::uint32_t height) {
    if (width == 0 || height == 0 || rgba.size() != static_cast<std::size_t>(width) * height * 4U) return {};
    const std::uint32_t half_width = half_of(width);
    const std::uint32_t half_height = half_of(height);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(half_width) * half_height * 4U);
    const std::size_t pitch = static_cast<std::size_t>(width) * 4U;
    for (std::uint32_t y = 0; y < half_height; ++y) {
        const std::uint32_t y0 = std::min(2U * y, height - 1U);
        const std::uint32_t y1 = std::min(2U * y + 1U, height - 1U);
        halve_row(rgba.data() + y0 * pitch, rgba.data() + y1 * pitch, width, result.data() + static_cast<std::size_t>(y) * half_width * 4U);
    }
    return result;
}

void renormalize_normals(const std::span<std::uint8_t> rgba) noexcept {
    // The shaders read a normal as 2 * (rgb / 255 - 0.5); a box average of unit normals is shorter than one.
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        float v[3];
        float length = 0.0F;
        for (std::size_t c = 0; c < 3; ++c) {
            v[c] = (static_cast<float>(rgba[i + c]) - 127.5F) / 127.5F;
            length += v[c] * v[c];
        }
        length = std::sqrt(length);
        if (!(length > 0.25F)) continue; // nearly cancelled: no direction to keep, leave the average
        for (std::size_t c = 0; c < 3; ++c) {
            const float scaled = 127.5F + (v[c] / length) * 127.5F;
            rgba[i + c] = static_cast<std::uint8_t>(std::clamp(scaled + 0.5F, 0.0F, 255.0F));
        }
    }
}

bool extend_mip_chain(Texture& texture, const ChainKind kind) {
    if (!mip_chain_incomplete(texture)) return false;
    const PixelFormat format = texture.format;
    std::uint32_t width = half_of(texture.mips.back().width);
    std::uint32_t height = half_of(texture.mips.back().height);
    std::vector<std::uint8_t> rgba = halve_stored_level(format, texture.mips.back());
    if (kind == ChainKind::normal && renormalize_normal_chains) renormalize_normals(rgba);
    std::vector<MipLevel> added;
    for (;;) {
        added.push_back(store_level(format, rgba, width, height));
        if (width == 1U && height == 1U) break;
        rgba = box_halve_rgba(rgba, width, height);
        if (kind == ChainKind::normal && renormalize_normal_chains) renormalize_normals(rgba);
        width = half_of(width);
        height = half_of(height);
    }
    texture.source.authored_levels = static_cast<std::uint8_t>(texture.mips.size());
    texture.mips.reserve(texture.mips.size() + added.size());
    for (MipLevel& level : added) texture.mips.push_back(std::move(level));
    return true;
}

} // namespace eawr::assets
