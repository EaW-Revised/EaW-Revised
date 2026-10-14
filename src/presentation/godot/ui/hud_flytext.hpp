#pragma once

#include "ui/hud_icon_text.hpp"
#include "eawr/presentation/ui/hud_flytext.hpp"

namespace eawr::presentation::godot_backend {

// HCB-64: reuse the shared authored readout's font, text cell and drawing path.
// No game-state lookup, award inference or clock advancement belongs here.
struct HudFlyTextRenderer final {
    HudIconTextRenderer readout;
    std::string retained_text;
    std::optional<presentation::ui::HudFlyTextPresentation> presented;

    void update(godot::Control& parent, const presentation::ui::HudIconText& style,
                const presentation::ui::HudFlyTextView& view, const std::uint64_t frame,
                const presentation::ui::ReferenceSpace& space) {
        namespace model = presentation::ui;
        presented = model::hud_flytext_presentation(view, frame);
        retained_text = presented ? std::string(presented->text) : std::string{};
        if (presented) presented->text = retained_text;
        const auto position = presented ? presented->position : eawr::assets::Vec2f{};
        const auto value = presented ? godot::String::utf8(presented->text.data(),
            static_cast<int64_t>(presented->text.size())) : godot::String{};
        readout.update(parent, style, value, {position.x, position.y, space.scale}, space,
            presented ? static_cast<float>(presented->alpha) / 255.0F : 0.0F, true);
    }
};

} // namespace eawr::presentation::godot_backend
