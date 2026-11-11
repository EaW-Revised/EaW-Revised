#pragma once

#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/ui/shell_anchors.hpp"
#include <string>

namespace eawr::viewer::galactic {
// Text-button faces prefer Blank, falling back to Icon when no Blank is authored.
[[nodiscard]] inline std::string command_bar_text_button_face(const data::ui::CommandBarComponent& style) {
    const auto blank = style.list(data::ui::Field::blank_texture_name);
    if (!blank.empty()) return blank.front();
    const auto icon = style.list(data::ui::Field::icon_texture_name);
    return icon.empty() ? std::string{} : icon.front();
}
// Command-bar artwork combines the authored component and viewport scales.
[[nodiscard]] inline float command_bar_component_scale(const data::ui::CommandBarComponent& style, float ui_scale) {
    return ui_scale * style.number(data::ui::Field::scale).value_or(1.0F);
}
// A bone-only text button uses its authored face dimensions around the bone origin.
[[nodiscard]] inline data::ui::ShellAnchor bone_button_anchor(const data::ui::ShellBone& bone,
    float width, float height, float scale) {
    data::ui::ShellAnchor anchor;
    anchor.name = bone.name;
    anchor.origin = bone.origin;
    width *= scale;
    height *= scale;
    anchor.rect = {bone.origin.x - width * 0.5F, bone.origin.y - height * 0.5F, width, height};
    return anchor;
}
// State artwork is sized in reference units, before shell_to_screen applies the viewport scale.
// It is centred on the component's bone, as in the shared button-quad rule.
[[nodiscard]] inline data::ui::ReferenceRect button_face_rect(const data::ui::ShellAnchor& anchor,
    float width, float height, float scale) {
    const auto origin = anchor.origin.value_or(eawr::assets::Vec2f{
        anchor.rect.x + anchor.rect.width * 0.5F, anchor.rect.y + anchor.rect.height * 0.5F});
    return bone_button_anchor({anchor.name, origin}, width, height, scale).rect;
}

}
