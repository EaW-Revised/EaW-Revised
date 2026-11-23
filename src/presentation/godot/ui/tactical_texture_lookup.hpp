#pragma once

#include "eawr/presentation/ui/theme.hpp"

namespace eawr::presentation::godot_backend {
enum class TacticalAssetRequirement { required, optional };
// Warning suppression and optional asset probing are independent policies.
inline ui::ThemeTexture tactical_texture_lookup(std::string_view name, const assets::MegaTexture* atlas,
    const ui::StandaloneTextures& required, const ui::StandaloneTextures& optional,
    TacticalAssetRequirement requirement = TacticalAssetRequirement::required) {
    return ui::resolve_ui_texture(name, atlas, requirement == TacticalAssetRequirement::optional ? optional : required);
}
} // namespace eawr::presentation::godot_backend
