#pragma once

#include <algorithm>

namespace eawr::presentation::ui {

// #8366 (#7956): where an icon texture is drawn inside the box of the slot that holds it. Every slot that draws an
// icon (build bar and tray, queue strip, ground pads and structure slots, land tray, orbit row, hero actions, the
// black market, corruption and destruction slots, ...) uses this one rule; none stretches the art over the box.
// The icon always keeps its own aspect ratio and is centred in the box. The mode says how big it is drawn:
//   natural  its texture size times the authored scale (the build bar, tray and queue strip: GHUD-S74);
//   shrink   natural, then scaled down (never up) until both sides fit the box;
//   contain  the largest size that fits the box, up or down (a square icon in a square slot fills it, a 20 x 40
//            icon in a square slot is 1:2 and as tall as the slot).
enum class IconFitMode { natural, shrink, contain };

struct IconFitRect final {
    float x{}, y{}, width{}, height{};
    [[nodiscard]] friend constexpr bool operator==(const IconFitRect&, const IconFitRect&) noexcept = default;
};

[[nodiscard]] inline IconFitRect fit_icon_rect(float box_width, float box_height, float texture_width,
    float texture_height, float scale, IconFitMode mode) noexcept {
    float width = texture_width * scale, height = texture_height * scale;
    if (mode != IconFitMode::natural && texture_width > 0.0F && texture_height > 0.0F && box_width > 0.0F
        && box_height > 0.0F) {
        const float contain = std::min(box_width / texture_width, box_height / texture_height);
        const float factor = mode == IconFitMode::contain ? contain : std::min(scale, contain);
        width = texture_width * factor; height = texture_height * factor;
    }
    return {(box_width - width) * 0.5F, (box_height - height) * 0.5F, width, height};
}

}
