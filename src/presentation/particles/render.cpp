#include "render_internal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

// Quad construction follows the MIT-licensed alo-viewer revision
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4 (DirectX9/ParticleRenderers.cpp):
// corner order, the 0-1-2 / 2-1-3 index pattern, the texture-coordinate
// assignment. Legacy kite geometry follows MD-07. Blend, depth-write and phase policy
// are taken from the render state of the public Engine/Prim* effects that the
// legacy selector names; see docs/reports/P1-08-rendering.md.

namespace eawr::presentation::particles {
using namespace render_detail;

float heat_distortion_pixel_change_bound(const float peak_vertex_alpha,
    const float distortion_amount, const std::int32_t width, const std::int32_t height) noexcept {
    // The heat shader draws blend_mix with ALPHA = texel alpha x vertex alpha
    // (at most a) and ALBEDO = the screen copy sampled with filter_linear at
    // SCREEN_UV + ALPHA x distortion x (a texel offset in [-1, 1] per axis).
    // So the sample moves at most a x distortion x width pixels in x and
    // a x distortion x height in y. A bilinear sample offset by fx, fy (each
    // clamped to one pixel, plus one step of 8-bit filter weight precision)
    // keeps weight (1 - fx)(1 - fy) on its own texel, so it differs from the
    // unshifted pixel by at most w = 1 - (1 - fx)(1 - fy) per channel. With
    // the covered pixel equal to the copy, blend_mix changes it by
    // ALPHA x (sample - pixel), at most a x w. The viewer's linear tonemap
    // without post effects keeps the copy, the blend and the capture in the
    // same 8-bit encoded values, and the store rounds to nearest (allowing
    // 0.6 of a step of conversion error), so each channel moves by at most
    // floor(255 a w + 0.6) whole steps. The bound sums three channels.
    const float alpha = std::clamp(peak_vertex_alpha, 0.0F, 1.0F);
    if (!(alpha > 0.0F)) return 0.0F;
    constexpr float filter_step = 1.0F / 256.0F;
    const float reach = alpha * distortion_amount;
    const float fx = std::min(1.0F, reach * static_cast<float>(std::max(width, 0)) + filter_step);
    const float fy = std::min(1.0F, reach * static_cast<float>(std::max(height, 0)) + filter_step);
    const float neighbour_weight = 1.0F - (1.0F - fx) * (1.0F - fy);
    const float steps = std::floor(255.0F * alpha * neighbour_weight + 0.6F);
    return 3.0F * steps / 255.0F;
}


std::string_view to_string(const EffectDetachState state) noexcept {
    switch (state) {
    case EffectDetachState::draining: return "draining";
    case EffectDetachState::released: return "released";
    }
    return "invalid";
}


std::string hex64(const std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t index = 0; index < 16; ++index) {
        text[15 - index] = digits[(value >> (index * 4)) & 15U];
    }
    return text;
}


} // namespace eawr::presentation::particles
