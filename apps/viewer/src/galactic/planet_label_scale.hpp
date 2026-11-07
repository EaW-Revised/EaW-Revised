#pragma once

#include "eawr/data/ui/command_bar.hpp"
#include <algorithm>
#include <cmath>

namespace eawr::viewer::galactic {
struct PlanetLabelBounds final { float x{}, y{}, width{}, height{}; };
struct PlanetLabelPlacement final { float x{}, y{}, scale{}; PlanetLabelBounds bounds; };
// Offset Y is unused; the default horizontal and vertical anchors remain unverified.
[[nodiscard]] inline PlanetLabelPlacement planet_label_placement(const data::ui::CommandBarComponent& style,
    float anchor_x, float anchor_y, float ui_scale, float measured_width, float measured_height,
    float control_width, float control_height) {
    const auto offset = style.vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    const auto text = style.vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
    const float scale = style.number(data::ui::Field::scale).value_or(1.0F);
    const float justification = style.flag(data::ui::Field::left_justified) ? 0.0F
        : (style.flag(data::ui::Field::right_justified) ? 1.0F : 0.5F);
    const float x = anchor_x + (offset.x + text.x) * ui_scale - measured_width * scale * justification;
    const float y = anchor_y - text.y * ui_scale - measured_height * scale * 0.5F;
    const float w = control_width * scale, h = control_height * scale;
    return {x, y, scale, {x + std::min(0.0F, w), y + std::min(0.0F, h), std::abs(w), std::abs(h)}};
}
}
