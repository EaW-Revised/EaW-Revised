#pragma once

#include "eawr/assets/assets.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace eawr::assets {

// Mip chains for textures that stop above 1x1.
//
// A mip chain is the set of smaller copies a GPU samples when the surface is far away. Some mod hull textures ship as a
// single level, so a ship seen from afar samples the full-size texture at 16-64 texels per screen pixel and its surface
// shimmers (issue #5989). Retail asks its texture loader for a full chain and averages each level with a plain box filter
// on the stored 8-bit values (no colour-space conversion); this does the same for the textures the caller names.
//
// The levels are produced in the texture's own format from a 2x2 box average of the decoded stored values (round to
// nearest) and, for the block-compressed formats, re-encoded. The encoder is this project's own, written from the public
// S3TC block layout: it uses no third-party code and none of retail's.
//
// Pure CPU work on the texture it is given: no I/O, no globals, no Godot. Safe on any thread, one texture per call.

// What the texture holds, which decides how a smaller level is made.
enum class ChainKind : std::uint8_t {
    colour, // the box average as it is
    compensated_light, // match generated levels from level 2 to the top-level luminance ranks
    // A tangent-space normal map (rgb = xyz as 2 * (rgb / 255 - 0.5), alpha untouched, e.g. gloss).
    normal,
};

// Whether the levels of a normal map are scaled back to unit length after each box average. Off: a normal map gets the plain
// box average like any other texture, which is what retail's chain request (mip filter 5, box) does and what the rig arm
// measured (#5989: blink events 102 to 37). On, the far levels keep full-length normals and so their full specular peaks,
// which nobody has measured and which may bring hull specks back. This constant is the one place that decides.
inline constexpr bool renormalize_normal_chains = false;

// True when `texture` is of a format extend_mip_chain completes (BC2, BC3, BC1 without 1-bit alpha, and the 24- and 32-bit
// uncompressed formats), is well formed (levels tightly packed, each the floor-half of the one before) and its chain stops
// above 1x1: a single level or an authored partial chain. False for everything else, including a finished chain.
[[nodiscard]] bool mip_chain_incomplete(const Texture& texture) noexcept;

// Appends the missing levels down to 1x1 after the last authored level. The authored levels stay byte for byte as they
// were; each new level is the box average of the one before it. Returns false, leaving `texture` as it was, when
// mip_chain_incomplete(texture) is false. A dimension that is odd at some level drops its last column or row from the
// average (floor halving, as the level sizes themselves do).
bool extend_mip_chain(Texture& texture, ChainKind kind);

// Bytes extend_mip_chain would add (0 when it does not apply).
[[nodiscard]] std::uint64_t mip_chain_added_bytes(const Texture& texture) noexcept;

// Block-level helpers, exposed for the tests and for tools that compare against a reference.
//
// Decodes a tightly packed BC1, BC2 or BC3 level of `width` x `height` texels to RGBA8 (4 bytes per texel, row-major).
// BC1 decodes opaque (a three-colour block's fourth value is black). Empty for another format or too few bytes.
[[nodiscard]] std::vector<std::uint8_t> decode_blocks(PixelFormat format, std::span<const std::byte> blocks, std::uint32_t width,
                                                      std::uint32_t height);

// Encodes `width` x `height` RGBA8 texels (row-major, `rgba.size() == width * height * 4`) to tightly packed BC1, BC2 or
// BC3. Texels past the right and bottom edge of a partial block repeat the edge texel. BC1 drops alpha.
[[nodiscard]] std::vector<std::byte> encode_blocks(PixelFormat format, std::span<const std::uint8_t> rgba, std::uint32_t width,
                                                   std::uint32_t height);

// The first halving as the chain generator does it: reads `level` (of `format`) one block row, or two rows, at a time, so
// the full-size RGBA image is never held, and returns the 2x2 box average at max(1, w/2) x max(1, h/2). Equal to
// box_halve_rgba of the whole decoded level (tested). Empty when `level` is not tightly packed or the format is not handled.
[[nodiscard]] std::vector<std::uint8_t> halve_level_rgba(PixelFormat format, const MipLevel& level);

// 2x2 box average of `rgba` (`width` x `height`) down to max(1, w/2) x max(1, h/2), round to nearest.
[[nodiscard]] std::vector<std::uint8_t> box_halve_rgba(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height);

// Scales the rgb of every texel of `rgba` to a unit normal (see ChainKind::normal); alpha is untouched. A texel whose
// vector is shorter than a quarter of a unit has no usable direction and stays as it is.
void renormalize_normals(std::span<std::uint8_t> rgba) noexcept;

} // namespace eawr::assets
