#include "eawr/assets/texture_chain.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #5989: compressed mip chains for BC3 textures that stop above 1x1. Pure CPU; no game data.

namespace {

using namespace eawr::assets;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

// A light-texture look: near black, a grid of small bright windows in a few colours, a dim hull pattern, alpha 255.
std::vector<std::uint8_t> light_image(const std::uint32_t width, const std::uint32_t height) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* const texel = &rgba[(static_cast<std::size_t>(y) * width + x) * 4U];
            const bool window = (x % 13U) < 3U && (y % 11U) < 2U;
            const std::uint32_t kind = ((x / 13U) + (y / 11U)) % 3U;
            if (window) {
                texel[0] = static_cast<std::uint8_t>(kind == 0 ? 250U : (kind == 1 ? 90U : 255U));
                texel[1] = static_cast<std::uint8_t>(kind == 0 ? 220U : (kind == 1 ? 200U : 120U));
                texel[2] = static_cast<std::uint8_t>(kind == 0 ? 120U : (kind == 1 ? 255U : 40U));
            } else {
                const auto dim = static_cast<std::uint8_t>(((x * 7U + y * 3U) % 9U));
                texel[0] = dim;
                texel[1] = dim;
                texel[2] = static_cast<std::uint8_t>(dim / 2U);
            }
            texel[3] = 255;
        }
    }
    return rgba;
}

Texture bc3_texture(const std::uint32_t width, const std::uint32_t height, const std::vector<std::uint8_t>& rgba) {
    Texture texture;
    texture.width = width;
    texture.height = height;
    texture.format = PixelFormat::bc3;
    texture.has_alpha = true;
    MipLevel level;
    level.width = width;
    level.height = height;
    level.row_pitch = ((width + 3U) / 4U) * 16U;
    level.bytes = encode_blocks(PixelFormat::bc3, rgba, width, height);
    texture.mips.push_back(std::move(level));
    return texture;
}

// Mean of R+G+B over a decoded level (0..765).
double energy(const std::vector<std::uint8_t>& rgba) {
    double sum = 0.0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
    return sum / static_cast<double>(rgba.size() / 4U);
}

// A plain box filter written independently of the library, in doubles: the reference the energy bound is stated against.
struct Plane final {
    std::uint32_t width{}, height{};
    std::vector<double> rgb; // 3 per texel
};
Plane reference_level_zero(const std::vector<std::uint8_t>& rgba, const std::uint32_t width, const std::uint32_t height) {
    Plane plane{width, height, std::vector<double>(static_cast<std::size_t>(width) * height * 3U)};
    for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i)
        for (std::size_t c = 0; c < 3; ++c) plane.rgb[i * 3U + c] = rgba[i * 4U + c];
    return plane;
}
Plane reference_halve(const Plane& in) {
    Plane out;
    out.width = std::max(1U, in.width / 2U);
    out.height = std::max(1U, in.height / 2U);
    out.rgb.assign(static_cast<std::size_t>(out.width) * out.height * 3U, 0.0);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const std::uint32_t x0 = std::min(2U * x, in.width - 1U), x1 = std::min(2U * x + 1U, in.width - 1U);
            const std::uint32_t y0 = std::min(2U * y, in.height - 1U), y1 = std::min(2U * y + 1U, in.height - 1U);
            for (std::size_t c = 0; c < 3; ++c) {
                const auto at = [&](const std::uint32_t px, const std::uint32_t py) { return in.rgb[(static_cast<std::size_t>(py) * in.width + px) * 3U + c]; };
                out.rgb[(static_cast<std::size_t>(y) * out.width + x) * 3U + c] = (at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1)) / 4.0;
            }
        }
    }
    return out;
}
double reference_energy(const Plane& plane) {
    double sum = 0.0;
    for (const double value : plane.rgb) sum += value;
    return sum / static_cast<double>(plane.rgb.size() / 3U);
}

void known_block_decodes() {
    // Alpha endpoints 255/0, all alpha indices 0 (255). Colours red (0xF800) and blue (0x001F); colour indices
    // 0,1,2,3 for the first four texels, 0 after.
    std::vector<std::byte> block(16, std::byte{0});
    block[0] = std::byte{255};
    block[8] = std::byte{0x00};
    block[9] = std::byte{0xF8};
    block[10] = std::byte{0x1F};
    block[11] = std::byte{0x00};
    block[12] = std::byte{0b11100100}; // texels 0..3: 0, 1, 2, 3
    const auto rgba = decode_blocks(PixelFormat::bc3, block, 4, 4);
    expect(rgba.size() == 64, "a 4x4 level decodes to 64 bytes");
    if (rgba.size() != 64) return;
    expect(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255, "index 0 is the first endpoint (red)");
    expect(rgba[4] == 0 && rgba[5] == 0 && rgba[6] == 255, "index 1 is the second endpoint (blue)");
    expect(rgba[8] == 170 && rgba[10] == 85, "index 2 is two thirds of the first endpoint plus a third of the second");
    expect(rgba[12] == 85 && rgba[14] == 170, "index 3 is a third of the first endpoint plus two thirds of the second");
    // 8-value alpha mode: a0 = 255 > a1 = 0, index 2 is (6 * 255 + 0) / 7 = 218.
    block[2] = std::byte{0b010}; // texel 0 alpha index 2
    expect(decode_blocks(PixelFormat::bc3, block, 4, 4)[3] == 218, "alpha index 2 of an eight-value block is (6 * a0 + a1) / 7");
    // 6-value alpha mode: a0 = 0 <= a1 = 255, index 6 is 0 and index 7 is 255.
    block[0] = std::byte{0};
    block[1] = std::byte{255};
    block[2] = std::byte{0b110 | (0b111 << 3)}; // texel 0 index 6, texel 1 index 7
    const auto six = decode_blocks(PixelFormat::bc3, block, 4, 4);
    expect(six[3] == 0 && six[7] == 255, "alpha indices 6 and 7 of a six-value block are 0 and 255");
}

void encoder_round_trips_flat_and_ramp_blocks() {
    std::vector<std::uint8_t> flat(4 * 4 * 4, 0);
    for (std::size_t i = 0; i < 16; ++i) {
        flat[i * 4] = 0;
        flat[i * 4 + 1] = 0;
        flat[i * 4 + 2] = 0;
        flat[i * 4 + 3] = 255;
    }
    expect(decode_blocks(PixelFormat::bc3, encode_blocks(PixelFormat::bc3, flat, 4, 4), 4, 4) == flat, "a black opaque block survives encoding exactly");
    // A ramp in grey: four palette values cover 16 levels, so a texel is at most half a palette step (about 40) off.
    std::vector<std::uint8_t> ramp(4 * 4 * 4, 255);
    for (std::size_t i = 0; i < 16; ++i)
        for (std::size_t c = 0; c < 3; ++c) ramp[i * 4 + c] = static_cast<std::uint8_t>(i * 16U);
    const auto decoded = decode_blocks(PixelFormat::bc3, encode_blocks(PixelFormat::bc3, ramp, 4, 4), 4, 4);
    int worst = 0;
    for (std::size_t i = 0; i < ramp.size(); ++i) worst = std::max(worst, std::abs(static_cast<int>(ramp[i]) - static_cast<int>(decoded[i])));
    expect(worst <= 41, "a grey ramp block encodes within half a palette step (41/255) of every texel");
}

// The bound the PR states: per level, mean R+G+B of the compressed level within 5% of the box-filter reference plus 6
// units of the 0..765 sum scale (2 stored values per channel: the 565 endpoint grid steps by 8, 4 and 8).
void generated_chain_matches_the_box_reference() {
    constexpr std::uint32_t side = 256;
    const auto image = light_image(side, side);
    Texture texture = bc3_texture(side, side, image);
    const std::vector<std::byte> original = texture.mips[0].bytes;
    expect(mip_chain_incomplete(texture), "a single-level BC3 texture needs a chain");
    const std::uint64_t predicted = mip_chain_added_bytes(texture);
    expect(extend_mip_chain(texture, ChainKind::colour), "the chain is generated");
    expect(texture.mips.size() == 9, "256 x 256 gets nine levels down to 1 x 1");
    expect(texture.mips[0].bytes == original, "the authored top level is byte for byte unchanged");
    expect(texture.source.authored_levels == 1, "the source records that one level is the file's");
    std::uint64_t added = 0;
    for (std::size_t i = 1; i < texture.mips.size(); ++i) added += texture.mips[i].bytes.size();
    expect(added == predicted, "bc3_chain_added_bytes matches what was added");
    expect(!mip_chain_incomplete(texture) && !extend_mip_chain(texture, ChainKind::colour), "a finished chain is left alone");
    Texture untouched = bc3_texture(16, 16, light_image(16, 16));
    untouched.mips.clear();
    expect(!extend_mip_chain(untouched, ChainKind::colour) && untouched.source.authored_levels == 0, "a refused texture records nothing");

    Plane reference = reference_level_zero(decode_blocks(PixelFormat::bc3, texture.mips[0].bytes, side, side), side, side);
    double worst_relative = 0.0, worst_error = 0.0;
    for (std::size_t level = 1; level < texture.mips.size(); ++level) {
        reference = reference_halve(reference);
        const MipLevel& mip = texture.mips[level];
        expect(mip.width == reference.width && mip.height == reference.height, "level sizes halve down to 1 x 1");
        expect(mip.row_pitch == ((mip.width + 3U) / 4U) * 16U && mip.bytes.size() == mip.row_pitch * ((mip.height + 3U) / 4U),
               "levels are tightly packed blocks");
        const double want = reference_energy(reference);
        const auto decoded = decode_blocks(PixelFormat::bc3, mip.bytes, mip.width, mip.height);
        const double got = energy(decoded);
        const double allowed = 0.05 * want + 6.0;
        // The mean alone passes an all-black chain on a sparse texture and cannot see misplaced values, so also: the
        // mean absolute error per channel against the reference level, and the brightest value.
        double error = 0.0, peak_want = 0.0, peak_got = 0.0;
        for (std::size_t k = 0; k < static_cast<std::size_t>(mip.width) * mip.height; ++k) {
            for (std::size_t c = 0; c < 3; ++c) {
                error += std::abs(decoded[k * 4U + c] - reference.rgb[k * 3U + c]);
                peak_want = std::max(peak_want, reference.rgb[k * 3U + c]);
                peak_got = std::max(peak_got, static_cast<double>(decoded[k * 4U + c]));
            }
        }
        error /= static_cast<double>(static_cast<std::size_t>(mip.width) * mip.height * 3U);
        worst_error = std::max(worst_error, error);
        const std::string where = " (level " + std::to_string(level) + ")";
        expect(error <= 6.0, "mean absolute error per channel against the box reference is at most 6/255" + where);
        if (want > 10.0) expect(got >= 0.75 * want && got <= 1.25 * want, "a level with energy keeps at least three quarters of it" + where);
        if (peak_want >= 40.0) expect(peak_got >= 0.6 * peak_want && peak_got <= 1.4 * peak_want, "the brightest value stays near the reference's" + where);
        worst_relative = std::max(worst_relative, std::abs(got - want) / std::max(want, 1.0));
        expect(std::abs(got - want) <= allowed, "level energy (mean R+G+B) is within 5% + 6 of the box-filter reference (level "
                                                    + std::to_string(level) + ": got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
    }
    std::cout << "texture_chain: worst relative energy error over the chain " << worst_relative
              << ", worst mean absolute error per channel " << worst_error << '\n';
}

// The first halving decodes block rows on its own path; it must equal the plain box filter of the decoded level.
void first_halving_equals_the_plain_box_filter() {
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes{{256, 256}, {100, 60}, {64, 1}, {1, 64}, {7, 5}, {4, 4},
                                                                      {5, 3}, {3, 9}, {2, 2}, {1, 1}, {17, 33}, {12, 7}, {8, 6}};
    for (const auto& [width, height] : sizes) {
        const Texture texture = bc3_texture(width, height, light_image(width, height));
        const auto streamed = halve_level_rgba(PixelFormat::bc3, texture.mips[0]);
        const auto plain = box_halve_rgba(decode_blocks(PixelFormat::bc3, texture.mips[0].bytes, width, height), width, height);
        expect(!streamed.empty() && streamed == plain,
               "the block-row halving equals the box filter of the decoded level (" + std::to_string(width) + " x " + std::to_string(height) + ")");
    }
    MipLevel broken = bc3_texture(8, 8, light_image(8, 8)).mips[0];
    broken.bytes.pop_back();
    expect(halve_level_rgba(PixelFormat::bc3, broken).empty(), "a level that is not tightly packed gives nothing");
}

void authored_chain_is_untouched() {
    constexpr std::uint32_t side = 64;
    Texture texture = bc3_texture(side, side, light_image(side, side));
    expect(extend_mip_chain(texture, ChainKind::colour), "setup: a full chain exists");
    const Texture before = texture;
    expect(!mip_chain_incomplete(texture), "a complete authored chain needs nothing");
    expect(!extend_mip_chain(texture, ChainKind::colour), "a complete authored chain is refused");
    bool same = texture.mips.size() == before.mips.size();
    for (std::size_t i = 0; same && i < texture.mips.size(); ++i) same = texture.mips[i].bytes == before.mips[i].bytes;
    expect(same, "a complete authored chain keeps every byte");
}

void partial_chain_keeps_its_authored_levels() {
    constexpr std::uint32_t side = 128;
    Texture full = bc3_texture(side, side, light_image(side, side));
    expect(extend_mip_chain(full, ChainKind::colour), "setup: a full chain exists");
    Texture partial = full;
    partial.mips.resize(3); // 128, 64, 32 authored
    const Texture authored = partial;
    expect(mip_chain_incomplete(partial), "an authored chain that stops at 32 px needs a tail");
    expect(extend_mip_chain(partial, ChainKind::colour), "the tail is generated");
    expect(partial.mips.size() == full.mips.size(), "the tail reaches 1 x 1");
    bool same = true;
    for (std::size_t i = 0; i < 3; ++i) same = same && partial.mips[i].bytes == authored.mips[i].bytes;
    expect(same, "the authored levels are byte for byte unchanged");
    double want = energy(decode_blocks(PixelFormat::bc3, authored.mips[2].bytes, 32, 32));
    for (std::size_t i = 3; i < partial.mips.size(); ++i) {
        const double got = energy(decode_blocks(PixelFormat::bc3, partial.mips[i].bytes, partial.mips[i].width, partial.mips[i].height));
        expect(std::abs(got - want) <= 0.05 * want + 6.0, "a tail level keeps the energy of the last authored level");
    }
}

void other_textures_are_untouched() {
    Texture bc5 = bc3_texture(16, 16, light_image(16, 16));
    bc5.format = PixelFormat::bc5;
    expect(!mip_chain_incomplete(bc5) && !extend_mip_chain(bc5, ChainKind::colour) && bc5.mips.size() == 1, "another block format is untouched");
    Texture broken = bc3_texture(16, 16, light_image(16, 16));
    broken.mips[0].bytes.pop_back();
    expect(!mip_chain_incomplete(broken) && !extend_mip_chain(broken, ChainKind::colour) && broken.mips.size() == 1, "a level of the wrong size is untouched");
    Texture empty;
    empty.format = PixelFormat::bc3;
    expect(!mip_chain_incomplete(empty) && !extend_mip_chain(empty, ChainKind::colour), "a texture without levels is untouched");
}

void odd_sizes_get_floor_halved_levels() {
    Texture texture = bc3_texture(100, 60, light_image(100, 60));
    expect(extend_mip_chain(texture, ChainKind::colour), "100 x 60 gets a chain");
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> want{{100, 60}, {50, 30}, {25, 15}, {12, 7}, {6, 3}, {3, 1}, {1, 1}};
    expect(texture.mips.size() == want.size(), "100 x 60 has seven levels");
    for (std::size_t i = 0; i < want.size() && i < texture.mips.size(); ++i)
        expect(texture.mips[i].width == want[i].first && texture.mips[i].height == want[i].second, "level sizes floor-halve");
    Texture strip = bc3_texture(64, 1, light_image(64, 1));
    expect(extend_mip_chain(strip, ChainKind::colour) && strip.mips.size() == 7 && strip.mips.back().width == 1 && strip.mips.back().height == 1,
           "a 64 x 1 strip halves in width only until 1 x 1");
}


// A tangent-space normal map with some roughness: normals tilted by a hash of the texel around +z, gloss in alpha.
std::vector<std::uint8_t> normal_image(const std::uint32_t width, const std::uint32_t height) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t h = x * 374761393U + y * 668265263U;
            h = (h ^ (h >> 13U)) * 1274126177U;
            h ^= h >> 16U;
            const float nx = (static_cast<float>(h & 255U) / 255.0F - 0.5F) * 2.4F;
            const float ny = (static_cast<float>((h >> 8U) & 255U) / 255.0F - 0.5F) * 2.4F;
            const float nz = 1.0F;
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            std::uint8_t* const texel = &rgba[(static_cast<std::size_t>(y) * width + x) * 4U];
            texel[0] = static_cast<std::uint8_t>(127.5F + nx / length * 127.5F + 0.5F);
            texel[1] = static_cast<std::uint8_t>(127.5F + ny / length * 127.5F + 0.5F);
            texel[2] = static_cast<std::uint8_t>(127.5F + nz / length * 127.5F + 0.5F);
            texel[3] = static_cast<std::uint8_t>(200U);
        }
    }
    return rgba;
}

// Mean length of the decoded normals of an RGBA level.
double mean_normal_length(const std::vector<std::uint8_t>& rgba) {
    double sum = 0.0;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        double length = 0.0;
        for (std::size_t c = 0; c < 3; ++c) {
            const double v = (static_cast<double>(rgba[i + c]) - 127.5) / 127.5;
            length += v * v;
        }
        sum += std::sqrt(length);
    }
    return sum / static_cast<double>(rgba.size() / 4U);
}

Texture block_texture(const PixelFormat format, const std::uint32_t width, const std::uint32_t height, const std::vector<std::uint8_t>& rgba) {
    Texture texture;
    texture.width = width;
    texture.height = height;
    texture.format = format;
    texture.has_alpha = format != PixelFormat::bc1;
    MipLevel level;
    level.width = width;
    level.height = height;
    level.row_pitch = ((width + 3U) / 4U) * (format == PixelFormat::bc1 ? 8U : 16U);
    level.bytes = encode_blocks(format, rgba, width, height);
    texture.mips.push_back(std::move(level));
    return texture;
}

void known_bc1_and_bc2_blocks_decode() {
    // BC1, c0 = red 0xF800 > c1 = blue 0x001F: four colours; indices 0..3 for the first four texels.
    std::vector<std::byte> bc1(8, std::byte{0});
    bc1[1] = std::byte{0xF8};
    bc1[2] = std::byte{0x1F};
    bc1[4] = std::byte{0b11100100};
    auto rgba = decode_blocks(PixelFormat::bc1, bc1, 4, 4);
    expect(rgba.size() == 64 && rgba[0] == 255 && rgba[6] == 255 && rgba[8] == 170 && rgba[10] == 85 && rgba[3] == 255,
           "BC1 four-colour mode: endpoints and thirds, opaque");
    // BC1, c0 = blue <= c1 = red: three colours and a black fourth; index 2 is the mean, index 3 black.
    bc1[0] = std::byte{0x1F};
    bc1[1] = std::byte{0x00};
    bc1[2] = std::byte{0x00};
    bc1[3] = std::byte{0xF8};
    rgba = decode_blocks(PixelFormat::bc1, bc1, 4, 4);
    expect(rgba[8] == 127 && rgba[10] == 127 && rgba[12] == 0 && rgba[13] == 0 && rgba[14] == 0,
           "BC1 three-colour mode: the mean, then black");
    // BC2: explicit alphas, nibble 0xF -> 255, 0x8 -> 136, 0x0 -> 0; colour as BC3 (four-colour mode always).
    std::vector<std::byte> bc2(16, std::byte{0});
    bc2[0] = std::byte{0x8F};
    bc2[8 + 1] = std::byte{0xF8};
    bc2[8 + 2] = std::byte{0x1F};
    const auto two = decode_blocks(PixelFormat::bc2, bc2, 4, 4);
    expect(two.size() == 64 && two[3] == 255 && two[7] == 136 && two[11] == 0, "BC2 alpha nibbles scale by 17");
}

// BC1 and BC2 get a chain whose energy follows the box reference, like BC3 does.
void other_block_formats_get_chains() {
    constexpr std::uint32_t side = 128;
    for (const PixelFormat format : {PixelFormat::bc1, PixelFormat::bc2}) {
        const std::string name = format == PixelFormat::bc1 ? "BC1" : "BC2";
        Texture texture = block_texture(format, side, side, light_image(side, side));
        const std::vector<std::byte> top = texture.mips[0].bytes;
        expect(mip_chain_incomplete(texture), name + ": a single-level texture needs a chain");
        const std::uint64_t predicted = mip_chain_added_bytes(texture);
        expect(extend_mip_chain(texture, ChainKind::colour) && texture.mips.size() == 8, name + ": 128 x 128 gets eight levels");
        expect(texture.mips[0].bytes == top, name + ": the top level is unchanged");
        std::uint64_t added = 0;
        for (std::size_t i = 1; i < texture.mips.size(); ++i) added += texture.mips[i].bytes.size();
        expect(added == predicted, name + ": the predicted added bytes are what was added");
        Plane reference = reference_level_zero(decode_blocks(format, texture.mips[0].bytes, side, side), side, side);
        for (std::size_t level = 1; level < texture.mips.size(); ++level) {
            reference = reference_halve(reference);
            const auto decoded = decode_blocks(format, texture.mips[level].bytes, texture.mips[level].width, texture.mips[level].height);
            const double want = reference_energy(reference);
            const double got = energy(decoded);
            expect(std::abs(got - want) <= 0.05 * want + 6.0, name + ": level energy within 5% + 6 of the box reference (level " + std::to_string(level) + ")");
            if (want > 10.0) expect(got >= 0.75 * want, name + ": a level with energy keeps it (level " + std::to_string(level) + ")");
        }
    }
    // BC1 with a 1-bit alpha cannot hold an averaged alpha: left alone.
    Texture with_alpha = block_texture(PixelFormat::bc1, 16, 16, light_image(16, 16));
    with_alpha.has_alpha = true;
    expect(!mip_chain_incomplete(with_alpha) && !extend_mip_chain(with_alpha, ChainKind::colour), "BC1 with alpha is untouched");
}

// The uncompressed formats: the chain is the exact box filter, channel order kept.
void uncompressed_formats_get_exact_chains() {
    constexpr std::uint32_t side = 32;
    const auto rgba = light_image(side, side);
    for (const PixelFormat format : {PixelFormat::rgba8, PixelFormat::bgra8, PixelFormat::bgr8}) {
        const std::uint32_t size = format == PixelFormat::bgr8 ? 3U : 4U;
        Texture texture;
        texture.width = side;
        texture.height = side;
        texture.format = format;
        texture.has_alpha = size == 4U;
        MipLevel level;
        level.width = side;
        level.height = side;
        level.row_pitch = side * size;
        for (std::size_t i = 0; i < static_cast<std::size_t>(side) * side; ++i) {
            const std::uint8_t* const in = &rgba[i * 4U];
            if (format == PixelFormat::rgba8) {
                for (std::size_t c = 0; c < 4; ++c) level.bytes.push_back(static_cast<std::byte>(in[c]));
            } else {
                level.bytes.push_back(static_cast<std::byte>(in[2]));
                level.bytes.push_back(static_cast<std::byte>(in[1]));
                level.bytes.push_back(static_cast<std::byte>(in[0]));
                if (size == 4U) level.bytes.push_back(static_cast<std::byte>(in[3]));
            }
        }
        texture.mips.push_back(level);
        const std::string name = format == PixelFormat::rgba8 ? "RGBA8" : (format == PixelFormat::bgra8 ? "BGRA8" : "BGR8");
        expect(extend_mip_chain(texture, ChainKind::colour) && texture.mips.size() == 6, name + ": 32 x 32 gets six levels");
        if (texture.mips.size() != 6) continue;
        expect(texture.mips[0].bytes == level.bytes, name + ": the top level is unchanged");
        // The reference: halve the RGBA source repeatedly with the plain filter and compare level 1 byte for byte.
        const auto want = box_halve_rgba(rgba, side, side);
        bool same = texture.mips[1].bytes.size() == static_cast<std::size_t>(side / 2U) * (side / 2U) * size;
        for (std::size_t i = 0; same && i < static_cast<std::size_t>(side / 2U) * (side / 2U); ++i) {
            const std::uint8_t* const w = &want[i * 4U];
            const std::byte* const got = &texture.mips[1].bytes[i * size];
            if (format == PixelFormat::rgba8) same = got[0] == std::byte{w[0]} && got[1] == std::byte{w[1]} && got[2] == std::byte{w[2]} && got[3] == std::byte{w[3]};
            else same = got[0] == std::byte{w[2]} && got[1] == std::byte{w[1]} && got[2] == std::byte{w[0]} && (size == 3U || got[3] == std::byte{w[3]});
        }
        expect(same, name + ": level 1 is the box filter of the source, channel order kept");
        expect(texture.mips.back().width == 1 && texture.mips.back().bytes.size() == size, name + ": the chain ends at one texel");
    }
}

// Normal maps: with renormalize_normal_chains off a normal map gets exactly the plain box chain; with it on, the vectors
// stay near unit length at every level where the plain average shortens them.
void normal_chain_follows_the_renormalise_switch() {
    constexpr std::uint32_t side = 128;
    const auto image = normal_image(side, side);
    Texture plain = block_texture(PixelFormat::bc3, side, side, image);
    Texture normal = plain;
    expect(extend_mip_chain(plain, ChainKind::colour) && extend_mip_chain(normal, ChainKind::normal), "both chains generate");
    expect(plain.mips[0].bytes == normal.mips[0].bytes, "the top level is the same for both");
    bool identical = plain.mips.size() == normal.mips.size();
    for (std::size_t i = 0; identical && i < plain.mips.size(); ++i) identical = plain.mips[i].bytes == normal.mips[i].bytes;
    expect(identical == !renormalize_normal_chains, "a normal map gets the plain box chain exactly when renormalising is off");
    const double top = mean_normal_length(decode_blocks(PixelFormat::bc3, normal.mips[0].bytes, side, side));
    double worst_normal = 1.0, shortest_plain = 1.0;
    for (std::size_t level = 2; level < normal.mips.size(); ++level) {
        const auto n = decode_blocks(PixelFormat::bc3, normal.mips[level].bytes, normal.mips[level].width, normal.mips[level].height);
        const auto p = decode_blocks(PixelFormat::bc3, plain.mips[level].bytes, plain.mips[level].width, plain.mips[level].height);
        const double length = mean_normal_length(n);
        worst_normal = std::min(worst_normal, length);
        shortest_plain = std::min(shortest_plain, mean_normal_length(p));
        if (renormalize_normal_chains) expect(length >= 0.92 && length <= 1.08, "a normal level keeps unit-length vectors (level " + std::to_string(level) + ": " + std::to_string(length) + ")");
        else expect(std::abs(length - mean_normal_length(p)) < 1e-9, "a plain normal level is the plain average (level " + std::to_string(level) + ")");
        // The gloss in alpha is averaged, not normalised: constant 200 stays near 200.
        expect(std::abs(static_cast<int>(n[3]) - 200) <= 8, "the alpha channel of a normal map stays the average of the gloss");
    }
    expect(top > 0.85, "setup: the source normals are close to unit length (BC3 noise included)");
    expect(shortest_plain < 0.9, "setup: the plain box average does shorten the normals at the coarse levels");
    std::cout << "texture_chain: normal levels " << worst_normal << " at worst against " << shortest_plain << " for the plain average\n";
    // renormalize_normals on its own: a short vector is scaled, a cancelled one is left, alpha stays.
    std::vector<std::uint8_t> texels{128, 128, 192, 77, 127, 128, 128, 55};
    renormalize_normals(texels);
    expect(texels[3] == 77 && texels[7] == 55, "alpha is not touched");
    expect(texels[0] == 128 && texels[1] == 128 && texels[2] == 255, "a short +z vector becomes a unit +z vector");
    expect(texels[4] == 127 && texels[5] == 128 && texels[6] == 128, "a vector with no length is left as it is");
}

void other_formats_stay_untouched() {
    Texture bc4;
    bc4.width = bc4.height = 8;
    bc4.format = PixelFormat::bc4;
    MipLevel level;
    level.width = level.height = 8;
    level.row_pitch = 16;
    level.bytes.assign(32, std::byte{1});
    bc4.mips.push_back(level);
    expect(!mip_chain_incomplete(bc4) && !extend_mip_chain(bc4, ChainKind::colour), "BC4 is untouched");
    Texture grey = bc4;
    grey.format = PixelFormat::l8;
    grey.mips[0].row_pitch = 8;
    grey.mips[0].bytes.assign(64, std::byte{1});
    expect(!mip_chain_incomplete(grey) && !extend_mip_chain(grey, ChainKind::normal), "an 8-bit texture is untouched");
}

void generation_is_deterministic() {
    Texture a = bc3_texture(64, 64, light_image(64, 64));
    Texture b = a;
    expect(extend_mip_chain(a, ChainKind::colour) && extend_mip_chain(b, ChainKind::colour), "both chains generate");
    bool same = a.mips.size() == b.mips.size();
    for (std::size_t i = 0; same && i < a.mips.size(); ++i) same = a.mips[i].bytes == b.mips[i].bytes;
    expect(same, "the same texture always gives the same chain");
}

} // namespace

int main() {
    known_block_decodes();
    known_bc1_and_bc2_blocks_decode();
    encoder_round_trips_flat_and_ramp_blocks();
    generated_chain_matches_the_box_reference();
    first_halving_equals_the_plain_box_filter();
    authored_chain_is_untouched();
    partial_chain_keeps_its_authored_levels();
    other_textures_are_untouched();
    odd_sizes_get_floor_halved_levels();
    generation_is_deterministic();
    other_block_formats_get_chains();
    uncompressed_formats_get_exact_chains();
    normal_chain_follows_the_renormalise_switch();
    other_formats_stay_untouched();
    if (failures != 0) {
        std::cerr << failures << " texture_chain check(s) failed\n";
        return 1;
    }
    std::cout << "texture_chain: ok\n";
    return 0;
}
