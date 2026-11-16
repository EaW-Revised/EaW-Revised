#include "eawr/presentation/ui/toggle_layout.hpp"

#include <algorithm>
#include <cmath>

namespace eawr::presentation::ui {

ToggleBox toggle_glyph_box(const ToggleSize off_art) noexcept { return {0.0F, 0.0F, off_art.width, off_art.height}; }

ToggleSize toggle_minimum_size(const ToggleSize authored, const ToggleSize off_art) noexcept {
    return {authored.width == 0.0F ? off_art.width : 0.0F, authored.height == 0.0F ? off_art.height : 0.0F};
}

ToggleBox toggle_caption_box(const ToggleBox& glyph, const bool push_style, const float control_width, const float gap) noexcept {
    if (push_style) return glyph;
    const float x = glyph.x + glyph.width + gap;
    return {x, glyph.y, std::max(0.0F, control_width - x), glyph.height};
}

float toggle_caption_gap(const ToggleBox& glyph) noexcept { return std::round(glyph.width * 0.3F); }

} // namespace eawr::presentation::ui
